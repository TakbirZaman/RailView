// ============================================================================
//  RailView - animated railway-station scene   (C++ / legacy OpenGL / GLUT)
//
//  Build:  Linux  : g++ Railview.cpp -o RailView -lglut -lGLU -lGL -ldl
//          macOS  : g++ Railview.cpp -o RailView -framework OpenGL -framework GLUT
//          Windows: link freeglut + opengl32 + glu32 + winmm (Code::Blocks / MSVC)
//
//  Keys:  D day | N night | A auto day-night cycle | R rain + lightning
//         SPACE pause | + / - time speed | S sound | B bloom
//         H toggle HUD | F fullscreen | ESC quit
//
//  Techniques: single time-of-day clock drives everything (ambient light tint,
//  sky gradient, sun/moon arcs, twilight), frame-rate independent animation,
//  additive-blend glows/light cones, atmospheric haze, letter-boxed viewport,
//  sun-cast shadows, wet-ground ripples, dawn/rain mist, passengers that walk
//  to the train and board it, GLSL bloom + colour grade (supersampled scene
//  buffer; auto-disables if the GPU has no GL 2.0 / FBO), and procedural
//  audio (rain, rumble, horn, thunder) synthesised at start-up - no assets.
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <mmsystem.h>
#endif
#ifdef __APPLE__
  #include <OpenGL/gl.h>
  #include <OpenGL/glu.h>
  #include <GLUT/glut.h>
  #include <dlfcn.h>
#else
  #include <GL/gl.h>
  #include <GL/glu.h>
  #include <GL/glut.h>
  #ifndef _WIN32
    #include <GL/glx.h>
  #endif
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define USE_MSAA 1                       // set to 0 if window creation fails
#define USE_POST 1                       // set to 0 to force the fixed-function path
#define USE_SOUND 1                      // set to 0 to drop the winmm audio layer
#define SSAA   1.5f                      // scene buffer scale (bloom path doubles as AA)

#ifndef GL_MULTISAMPLE
  #define GL_MULTISAMPLE 0x809D
#endif
#ifndef APIENTRY
  #define APIENTRY
#endif
#ifndef GL_CLAMP_TO_EDGE
  #define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
  #define GL_RGBA8 0x8058
#endif
#ifndef GL_FRAMEBUFFER_EXT
  #define GL_FRAMEBUFFER_EXT          0x8D40
  #define GL_COLOR_ATTACHMENT0_EXT    0x8CE0
  #define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#endif
#ifndef GL_VERTEX_SHADER
  #define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
  #define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
  #define GL_COMPILE_STATUS  0x8B81
#endif
#ifndef GL_LINK_STATUS
  #define GL_LINK_STATUS     0x8B82
#endif
#if USE_SOUND && defined(_WIN32) && defined(_MSC_VER)
  #pragma comment(lib, "winmm.lib")
#endif

// ------------------------------------------------------------------ utilities
static const float SW = 1200.f, SH = 720.f;      // logical scene size
static const float PI = 3.14159265f;

struct Col { float r, g, b; };
struct P   { float x, y; };

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf(float a, float b, float t)   { return a + (b - a) * t; }
static float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
static float frand()          { return rand() / (float)RAND_MAX; }
static float hash1(float n)   { float s = sinf(n * 12.9898f) * 43758.5453f; return s - floorf(s); }
static Col mixc(Col a, Col b, float t) { return Col{ lerpf(a.r,b.r,t), lerpf(a.g,b.g,t), lerpf(a.b,b.b,t) }; }
static Col scalec(Col c, float k)      { return Col{ clampf(c.r*k,0.f,1.f), clampf(c.g*k,0.f,1.f), clampf(c.b*k,0.f,1.f) }; }

// --------------------------------------------------------------- global state
struct State {
    float tod = 8.f;                 // clock, hours 0..24
    float todTarget = -1.f;          // fast-forward target (<0 = none)
    bool  autoCycle = false, paused = false, hud = true, rain = false, full = false;
    bool  audio = (USE_SOUND != 0), post = (USE_POST != 0);
    float timeScale = 1.f, simTime = 0.f;
    float overcast = 0.f, flash = 0.f, nextFlash = 4.f;
    float elevation = 1.f, daylight = 1.f, night = 0.f, twilight = 0.f;
    Col   amb {1.f,1.f,1.f};         // ambient tint applied to every lit colour
    Col   skyTop {0.f,0.f,0.f}, skyHor {0.f,0.f,0.f};
    float fps = 60.f;
} st;

static int winW = 1200, winH = 720, vpX = 0, vpY = 0, vpW = 1200, vpH = 720;
static int lastMs = 0;

struct Post {
    bool avail = false;
    GLuint fboS = 0, texS = 0, fboA = 0, texA = 0, fboB = 0, texB = 0;
    GLuint pBright = 0, pBlur = 0, pGrade = 0, pBloom = 0;
    int sw = 0, sh = 0, aw = 0, ah = 0;
};
static Post post;

// ------------------------------------------------------------------- lighting
static void updateLighting() {
    float e = sinf(PI * (st.tod - 6.f) / 12.f);        // sun elevation proxy
    st.elevation = e;
    st.daylight  = smoothstep(-0.14f, 0.30f, e);
    st.night     = 1.f - st.daylight;
    st.twilight  = clampf(1.f - fabsf(e - 0.04f) / 0.30f, 0.f, 1.f);

    Col nightC{0.20f,0.27f,0.52f}, dayC{1.f,1.f,1.f}, warm{1.00f,0.62f,0.46f};
    Col a = mixc(nightC, dayC, st.daylight);
    a = mixc(a, warm, st.twilight * 0.50f);
    float l = (a.r + a.g + a.b) / 3.f;
    a = mixc(a, Col{l,l,l}, 0.40f * st.overcast);                 // desaturate in rain
    float dark = 1.f - 0.28f * st.overcast;
    float fl = st.flash * st.flash * 0.6f;
    st.amb = Col{ clampf(a.r*dark+fl,0.f,1.f), clampf(a.g*dark+fl,0.f,1.f), clampf(a.b*dark+fl,0.f,1.f) };
}

static void col(float r, float g, float b, float a = 1.f) { glColor4f(r*st.amb.r, g*st.amb.g, b*st.amb.b, a); }
static void col(Col c, float a = 1.f)                     { col(c.r, c.g, c.b, a); }
static void raw(Col c, float a = 1.f)                     { glColor4f(c.r, c.g, c.b, a); }
static Col  lit(Col c)                                    { return Col{ c.r*st.amb.r, c.g*st.amb.g, c.b*st.amb.b }; }
static Col  hazeCol(Col c, float h)                       { return mixc(lit(c), st.skyHor, h); }

// ----------------------------------------------------------------- primitives
static void rect(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS); glVertex2f(x0,y0); glVertex2f(x1,y0); glVertex2f(x1,y1); glVertex2f(x0,y1); glEnd();
}
static void vrect(float x0, float y0, float x1, float y1, Col b, Col t) {   // lit vertical gradient
    glBegin(GL_QUADS);
    col(b); glVertex2f(x0,y0); glVertex2f(x1,y0);
    col(t); glVertex2f(x1,y1); glVertex2f(x0,y1);
    glEnd();
}
static void disc(float cx, float cy, float r, int seg = 32) {
    glBegin(GL_TRIANGLE_FAN); glVertex2f(cx, cy);
    for (int i = 0; i <= seg; ++i) { float a = 2.f*PI*i/seg; glVertex2f(cx + r*cosf(a), cy + r*sinf(a)); }
    glEnd();
}
static void halfDisc(float cx, float cy, float r, int seg = 60) {
    glBegin(GL_TRIANGLE_FAN); glVertex2f(cx, cy);
    for (int i = 0; i <= seg; ++i) { float a = PI*i/seg; glVertex2f(cx + r*cosf(a), cy + r*sinf(a)); }
    glEnd();
}
static void glow(float cx, float cy, float r, Col c, float k) {              // additive radial glow
    if (k < 0.01f) return;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(c.r, c.g, c.b, k); glVertex2f(cx, cy);
    glColor4f(c.r, c.g, c.b, 0.f);
    for (int i = 0; i <= 36; ++i) { float a = 2.f*PI*i/36; glVertex2f(cx + r*cosf(a), cy + r*sinf(a)); }
    glEnd();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
static void lightCone(float x, float y, float spread, float toY, float k, Col c) {
    if (k < 0.01f) return;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBegin(GL_TRIANGLES);
    glColor4f(c.r, c.g, c.b, k); glVertex2f(x, y);
    glColor4f(c.r, c.g, c.b, 0.f); glVertex2f(x - spread, toY); glVertex2f(x + spread, toY);
    glEnd();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
static void strokeCentered(const char* s, float cx, float cy, float width) {   // resolution independent text
    float len = (float)glutStrokeLength(GLUT_STROKE_ROMAN, (const unsigned char*)s);
    float sc = width / len;
    glPushMatrix();
    glTranslatef(cx - width*0.5f, cy - 50.f*sc, 0.f);
    glScalef(sc, sc, 1.f);
    glLineWidth(2.f);
    for (const char* p = s; *p; ++p) glutStrokeCharacter(GLUT_STROKE_ROMAN, *p);
    glPopMatrix();
}

// ------------------------------------------------------------------ entities
struct Star    { float x, y, size, phase, speed; };
struct Cloud   { float x, y, s, spd; };
struct Drop    { float x, y, v, len; };
enum { P_WAIT, P_TO_DOOR, P_TO_HOME };
struct Person  { float x, homeX; Col shirt; float ph, h; int st; float tx; bool aboard; };
struct Vehicle { float x, dir, speed, len; Col c; };
struct Train   { float x, v, vmax, dir, wheelY, stopX, timer, door; bool canStop, served; int state; };
enum { CRUISE, BRAKE, STOPPED, ACCELERATE, WAITING };

static const int NSTAR = 220, NCLOUD = 8, NDROP = 520, NGRAVEL = 500, NPERSON = 7;
static Star    stars[NSTAR];
static Cloud   clouds[NCLOUD];
static Drop    drops[NDROP];
static Person  ppl[NPERSON];
static Vehicle cars[3];
static float   gravX[NGRAVEL], gravY[NGRAVEL];
static Train   t1, t2;
static float   flockX = -200.f;
struct Meteor { bool on; float x, y, vx, vy, life; } met = { false, 0, 0, 0, 0, 0 };
static float   nextMeteor = 5.f;

static const float CAR_W = 120.f, GAP = 20.f, TRAIN_LEN = 696.f;
static const int   N_CARS = 5;

static float roadTop(float x) { return 449.f - 99.f  * x / 1210.f; }
static float roadBot(float x) { return 381.f - 113.f * x / 1210.f; }

// ---------------------------------------------- realism: shadows, wet, grass
struct Ripple { float x, y, age; };
static const int NRIP = 40, NGRASS = 420;
static Ripple ripples[NRIP];
static float  grassX[NGRASS], grassY[NGRASS];

static float shadowK() { float u = (st.tod - 6.f) / 12.f; return clampf((0.5f - u) * 3.f, -1.6f, 1.6f); }
static void shadow(float x, float baseY, float h, float width) {          // sun-direction cast shadow
    float a = 0.26f * st.daylight * (1.f - st.overcast);
    if (a < 0.01f) return;
    float k = shadowK(), cx = x + k * h * 0.5f, rx = width + fabsf(k) * h * 0.5f, ry = 3.f + 0.08f * rx;
    glColor4f(0.f, 0.f, 0.f, a);
    glBegin(GL_TRIANGLE_FAN); glVertex2f(cx, baseY);
    for (int i = 0; i <= 28; ++i) { float t = 2.f * PI * i / 28; glVertex2f(cx + rx * cosf(t), baseY + ry * sinf(t)); }
    glEnd();
}
static void drawGrass() {
    glPointSize(2.f);
    glBegin(GL_POINTS);
    for (int i = 0; i < NGRASS; ++i) { float k = hash1(i * 1.7f); col(0.12f + 0.25f * k, 0.40f + 0.25f * k, 0.10f + 0.12f * k, 0.55f); glVertex2f(grassX[i], grassY[i]); }
    glEnd();
}
static void drawMist() {                                                    // dawn/dusk and rain haze
    float m = 0.25f * st.twilight * (1.f - st.overcast * 0.5f) + 0.12f * st.overcast;
    if (m < 0.01f) return;
    glBegin(GL_QUADS);
    raw(st.skyHor, 0.f); glVertex2f(0,250); glVertex2f(SW,250);
    raw(st.skyHor, m);   glVertex2f(SW,340); glVertex2f(0,340);
    raw(st.skyHor, 0.f); glVertex2f(SW,480); glVertex2f(0,480);
    glEnd();
}
static void drawWet() {                                                     // sky sheen + ripples on rail bed
    if (st.overcast < 0.02f) return;
    glBegin(GL_QUADS);
    raw(st.skyHor, 0.12f * st.overcast); glVertex2f(0,0); glVertex2f(SW,0);
    raw(st.skyHor, 0.35f * st.overcast); glVertex2f(SW,250); glVertex2f(0,250);
    glEnd();
    glLineWidth(1.2f);
    for (int i = 0; i < NRIP; ++i) {
        const Ripple& r = ripples[i];
        if (r.age >= 1.f) continue;
        float rx = 4.f + r.age * 22.f;
        glColor4f(0.9f, 0.95f, 1.f, (1.f - r.age) * 0.5f * st.overcast);
        glBegin(GL_LINE_LOOP);
        for (int j = 0; j < 20; ++j) { float t = 2.f * PI * j / 20; glVertex2f(r.x + rx * cosf(t), r.y + rx * 0.3f * sinf(t)); }
        glEnd();
    }
}

// -------------------------------------------------------------------- sky etc
static void drawSky() {
    Col dayTop{0.18f,0.44f,0.86f}, dayHor{0.72f,0.87f,0.98f};
    Col nTop{0.008f,0.015f,0.07f}, nHor{0.07f,0.10f,0.24f};
    Col twTop{0.22f,0.24f,0.50f},  twHor{1.00f,0.52f,0.28f};
    Col top = mixc(nTop, dayTop, st.daylight);
    Col hor = mixc(nHor, dayHor, st.daylight);
    top = mixc(top, twTop, st.twilight * 0.55f);
    hor = mixc(hor, twHor, st.twilight * 0.80f);
    Col g = mixc(Col{0.035f,0.045f,0.07f}, Col{0.52f,0.55f,0.60f}, st.daylight);
    top = mixc(top, g, st.overcast * 0.85f);
    hor = mixc(hor, scalec(g, 1.2f), st.overcast * 0.85f);
    float f = st.flash * st.flash * 0.6f;
    top = mixc(top, Col{0.85f,0.88f,1.f}, f);
    hor = mixc(hor, Col{0.85f,0.88f,1.f}, f);
    st.skyTop = top; st.skyHor = hor;
    Col mid = mixc(hor, top, 0.45f);
    glBegin(GL_QUADS);
    raw(hor); glVertex2f(0,430); glVertex2f(SW,430);
    raw(mid); glVertex2f(SW,540); glVertex2f(0,540);
    raw(top); glVertex2f(SW,SH); glVertex2f(0,SH);
    glEnd();
}

static void drawStars() {
    float a = st.night * (1.f - st.overcast);
    if (a < 0.02f) return;
    for (int i = 0; i < NSTAR; ++i) {
        const Star& s = stars[i];
        float tw = 0.55f + 0.45f * sinf(st.simTime * s.speed + s.phase);
        float al = a * tw * smoothstep(450.f, 560.f, s.y);
        glPointSize(s.size);
        glBegin(GL_POINTS); glColor4f(1.f, 1.f, 0.92f, al); glVertex2f(s.x, s.y); glEnd();
    }
}

static void drawMeteor() {
    if (!met.on) return;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glLineWidth(2.f);
    glBegin(GL_LINES);
    glColor4f(1.f,1.f,1.f,0.f);       glVertex2f(met.x - met.vx*0.16f, met.y - met.vy*0.16f);
    glColor4f(1.f,1.f,1.f,met.life);  glVertex2f(met.x, met.y);
    glEnd();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void drawSun() {
    float s = (st.tod - 6.f) / 12.f;
    if (s < -0.12f || s > 1.12f) return;
    float h = sinf(PI * s);
    float x = 100.f + 1000.f * s, y = 445.f + 255.f * h;
    Col c = mixc(Col{1.0f,0.45f,0.12f}, Col{1.0f,0.97f,0.78f}, smoothstep(0.f, 0.55f, h));
    float vis = 1.f - st.overcast * 0.85f;
    glow(x, y, 330.f, c, 0.16f * vis);
    glow(x, y, 150.f, c, 0.45f * vis);
    raw(c, 1.f - st.overcast * 0.7f); disc(x, y, 28.f, 48);
}

static void drawMoon() {
    float m = fmodf(st.tod - 18.f + 24.f, 24.f) / 12.f;
    if (m > 1.5f) m -= 2.f;
    if (m < -0.12f || m > 1.12f) return;
    float x = 100.f + 1000.f * m, y = 445.f + 240.f * sinf(PI * m);
    float a = clampf(st.night * 1.4f, 0.f, 1.f) * (1.f - st.overcast * 0.8f);
    if (a < 0.02f) return;
    glow(x, y, 130.f, Col{0.55f,0.65f,1.f}, 0.30f * a);
    glColor4f(0.96f,0.96f,0.88f,a); disc(x, y, 25.f, 48);
    glColor4f(0.78f,0.80f,0.78f,a);                                  // craters
    disc(x-8.f, y+6.f, 5.f, 14); disc(x+7.f, y-5.f, 6.5f, 14); disc(x+4.f, y+10.f, 3.f, 10); disc(x-9.f, y-9.f, 3.5f, 10);
}

static void drawClouds() {
    static const float off[8][3] = {{0,0,25},{30,0,25},{-30,0,25},{-15,20,20},{15,20,20},{0,-15,15},{-45,10,20},{45,10,20}};
    Col base  = mixc(Col{1.f,1.f,1.f}, Col{0.62f,0.64f,0.68f}, st.overcast * 0.9f);
    Col shade = scalec(base, 0.78f);
    for (int i = 0; i < NCLOUD; ++i) {
        float am = (i < 5) ? 1.f : st.overcast;
        if (am < 0.02f) continue;
        const Cloud& c = clouds[i];
        col(shade, 0.95f * am);
        for (int k = 0; k < 8; ++k) disc(c.x + off[k][0]*c.s, c.y + (off[k][1]-7.f)*c.s, off[k][2]*c.s, 20);
        col(base, 0.95f * am);
        for (int k = 0; k < 8; ++k) disc(c.x + off[k][0]*c.s, c.y + off[k][1]*c.s, off[k][2]*c.s * 0.96f, 20);
    }
}

static void drawBirds() {
    float a = st.daylight * (1.f - st.overcast * 0.6f);
    if (a < 0.03f) return;
    glLineWidth(1.8f);
    for (int i = 0; i < 7; ++i) {
        int k = (i + 1) / 2; float side = (i % 2) ? 1.f : -1.f;
        float bx = flockX - k * 26.f, by = 620.f + side * k * 13.f + sinf(st.simTime * 0.4f) * 10.f;
        float f = sinf(st.simTime * 8.f + i * 0.7f);
        glBegin(GL_LINE_STRIP);
        col(0.08f, 0.08f, 0.10f, a);
        glVertex2f(bx-10.f, by+5.f*f); glVertex2f(bx-4.f, by+2.f+f); glVertex2f(bx, by);
        glVertex2f(bx+4.f, by+2.f+f);  glVertex2f(bx+10.f, by+5.f*f);
        glEnd();
    }
}

// ---------------------------------------------------------------------- hills
static const P HILL_FAR[] = {{0,450},{80,500},{190,560},{300,520},{420,600},{540,560},{680,640},{800,585},{930,650},{1060,600},{1200,540}};
static const P HILL_NEAR[] = {{0,450},{150,530},{200,555},{250,570},{300,565},{350,550},{450,570},{500,575},{600,580},{650,590},
                              {750,640},{800,650},{850,630},{900,640},{950,650},{1050,640},{1100,650},{1200,450}};

static void hillLayer(const P* p, int n, Col low, Col high, float haze) {       // column strips (concave-safe)
    glBegin(GL_QUADS);
    for (int i = 0; i < n - 1; ++i) {
        Col cb = hazeCol(low, haze);
        Col c0 = hazeCol(mixc(low, high, clampf((p[i].y   - 450.f) / 220.f, 0.f, 1.f)), haze);
        Col c1 = hazeCol(mixc(low, high, clampf((p[i+1].y - 450.f) / 220.f, 0.f, 1.f)), haze);
        raw(cb); glVertex2f(p[i].x, 450.f); glVertex2f(p[i+1].x, 450.f);
        raw(c1); glVertex2f(p[i+1].x, p[i+1].y);
        raw(c0); glVertex2f(p[i].x, p[i].y);
    }
    glEnd();
}

static void drawGround() {
    glBegin(GL_QUADS);
    raw(lit(Col{0.16f,0.50f,0.15f})); glVertex2f(0,250); glVertex2f(SW,250);
    raw(hazeCol(Col{0.38f,0.62f,0.28f}, 0.35f)); glVertex2f(SW,450); glVertex2f(0,450);
    glEnd();
}

static void drawRoad() {
    float xe = 1210.f;
    col(0.04f,0.04f,0.05f);
    glBegin(GL_QUADS); glVertex2f(0,roadTop(0)); glVertex2f(0,roadBot(0)); glVertex2f(xe,roadBot(xe)); glVertex2f(xe,roadTop(xe)); glEnd();
    col(0.28f,0.28f,0.30f);
    glBegin(GL_QUADS); glVertex2f(0,roadTop(0)-4.f); glVertex2f(0,roadBot(0)+4.f); glVertex2f(xe,roadBot(xe)+4.f); glVertex2f(xe,roadTop(xe)-4.f); glEnd();
    col(0.90f,0.90f,0.82f);
    glBegin(GL_QUADS);
    for (float x = 0.f; x < xe; x += 60.f) {
        float y0 = (roadTop(x) + roadBot(x)) * 0.5f, y1 = (roadTop(x+28.f) + roadBot(x+28.f)) * 0.5f;
        glVertex2f(x, y0-1.f); glVertex2f(x+28.f, y1-1.f); glVertex2f(x+28.f, y1+1.f); glVertex2f(x, y0+1.f);
    }
    glEnd();
}

static void drawVehicle(const Vehicle& v) {
    float T = roadTop(v.x), B = roadBot(v.x), w = T - B;
    float yc = (T + B) * 0.5f + (v.dir > 0 ? -0.2f : 0.2f) * w;
    float sc = w / 68.f * 0.9f, h = v.len * 0.5f;
    glPushMatrix();
    glTranslatef(v.x, yc, 0.f); glRotatef(-4.95f, 0.f, 0.f, 1.f); glScalef(sc * v.dir, sc, 1.f);
    glColor4f(0.f,0.f,0.f,0.3f); rect(-h, -1.f, h, 3.f);
    col(v.c);                  rect(-h, 3.f, h, 11.f);
    col(scalec(v.c, 1.2f));
    glBegin(GL_QUADS); glVertex2f(-h*0.45f,11.f); glVertex2f(h*0.40f,11.f); glVertex2f(h*0.22f,19.f); glVertex2f(-h*0.28f,19.f); glEnd();
    raw(mixc(lit(Col{0.5f,0.7f,0.85f}), Col{1.f,0.9f,0.55f}, st.night * 0.6f));
    glBegin(GL_QUADS); glVertex2f(-h*0.38f,11.5f); glVertex2f(h*0.34f,11.5f); glVertex2f(h*0.20f,18.f); glVertex2f(-h*0.24f,18.f); glEnd();
    col(0.05f,0.05f,0.06f); disc(-h*0.55f, 3.f, 4.2f, 12); disc(h*0.55f, 3.f, 4.2f, 12);
    glColor3f(1.f,0.95f,0.7f); disc(h-1.5f, 8.f, 1.8f, 8);
    glow(h + 2.f, 8.f, 16.f, Col{1.f,0.95f,0.7f}, 0.6f * st.night);
    glow(-h, 8.f, 10.f, Col{1.f,0.1f,0.05f}, 0.5f * st.night);
    glPopMatrix();
}

// ----------------------------------------------------------- trees / station
static void drawTree(float x, float sc, float ph) {
    float sw = sinf(st.simTime * 1.4f + ph) * (2.f + 5.f * st.overcast) + sinf(st.simTime * 3.1f + ph * 2.f) * 0.6f * st.overcast;
    auto S = [&](float y) { float k = y / 260.f; return sw * k * k; };
    shadow(x + 22.f * sc, 256.f, 240.f * sc, 22.f * sc);
    glPushMatrix(); glTranslatef(x, 255.f, 0.f); glScalef(sc, sc, 1.f);
    glBegin(GL_QUADS);
    col(0.42f,0.22f,0.09f); glVertex2f(0,0); glVertex2f(22,0); glVertex2f(21,80); glVertex2f(6,80);
    col(0.32f,0.16f,0.07f); glVertex2f(22,0); glVertex2f(45,0); glVertex2f(42,80); glVertex2f(21,80);
    glEnd();
    static const float bx0[3] = {-23.f,-20.f,-19.f}, bx1[3] = {68.f,65.f,65.f}, tx[3] = {30.f,28.f,27.f};
    for (int j = 0; j < 3; ++j) {
        float by = 80.f + 30.f * j, ty = 180.f + 30.f * j;
        glBegin(GL_TRIANGLES);
        col(0.20f,0.40f,0.15f); glVertex2f(bx0[j] + S(by), by); glVertex2f(bx1[j] + S(by), by);
        col(0.30f,0.54f,0.22f); glVertex2f(tx[j] + S(ty), ty);
        glEnd();
        glBegin(GL_TRIANGLES);                                            // shaded side
        col(0.12f,0.28f,0.10f, 0.45f); glVertex2f(tx[j] + S(ty), ty); glVertex2f(22.5f + S(by), by); glVertex2f(bx1[j] + S(by), by);
        glEnd();
    }
    glPopMatrix();
}

static void archWindow(float x, float y, float w, float h) {
    Col frame = lit(Col{0.30f,0.12f,0.10f});
    raw(frame); rect(x-2.f, y-2.f, x+w+2.f, y+h); halfDisc(x + w*0.5f, y+h, w*0.5f + 2.f, 20);
    raw(mixc(lit(Col{0.55f,0.76f,0.92f}), Col{1.f,0.84f,0.42f}, st.night));
    rect(x, y, x+w, y+h); halfDisc(x + w*0.5f, y+h, w*0.5f, 20);
    raw(frame);
    rect(x + w*0.5f - 0.8f, y, x + w*0.5f + 0.8f, y + h + w*0.5f);
    rect(x, y + h*0.5f - 0.8f, x + w, y + h*0.5f + 0.8f);
    glow(x + w*0.5f, y + h*0.5f, 48.f, Col{1.f,0.8f,0.4f}, 0.22f * st.night);
}

static void drawClock(float cx, float cy, float r) {
    raw(lit(Col{0.25f,0.10f,0.08f})); disc(cx, cy, r + 3.f, 40);
    raw(mixc(lit(Col{0.97f,0.96f,0.92f}), Col{1.f,0.94f,0.70f}, st.night)); disc(cx, cy, r, 40);
    raw(Col{0.1f,0.1f,0.1f}); glLineWidth(1.5f);
    glBegin(GL_LINES);
    for (int i = 0; i < 12; ++i) { float a = i * PI / 6.f; glVertex2f(cx + r*0.82f*cosf(a), cy + r*0.82f*sinf(a)); glVertex2f(cx + r*0.95f*cosf(a), cy + r*0.95f*sinf(a)); }
    glEnd();
    time_t tt = time(nullptr); struct tm* lt = localtime(&tt);
    float sec = (float)lt->tm_sec, mn = lt->tm_min + sec / 60.f, hr = (lt->tm_hour % 12) + mn / 60.f;
    float ang[3] = { PI/2.f - 2.f*PI*hr/12.f, PI/2.f - 2.f*PI*mn/60.f, PI/2.f - 2.f*PI*sec/60.f };
    float len[3] = { r*0.5f, r*0.75f, r*0.85f }, wid[3] = { 3.f, 2.f, 1.f };
    for (int i = 0; i < 3; ++i) {
        if (i == 2) raw(Col{0.8f,0.1f,0.1f}); else raw(Col{0.1f,0.1f,0.1f});
        glLineWidth(wid[i]);
        glBegin(GL_LINES); glVertex2f(cx, cy); glVertex2f(cx + len[i]*cosf(ang[i]), cy + len[i]*sinf(ang[i])); glEnd();
    }
    raw(Col{0.1f,0.1f,0.1f}); disc(cx, cy, 2.f, 10);
}

static void drawStation() {
    const float cx = 675.f, cy = 230.f;
    col(0.67f,0.60f,0.90f); halfDisc(cx, cy, 377.f, 120);
    col(0.60f,0.55f,0.50f); halfDisc(cx, cy, 375.f, 120);
    col(0.02f,0.02f,0.02f); halfDisc(cx, cy, 370.f, 120);
    glBegin(GL_TRIANGLE_FAN);                                                   // domed wall shading
    col(0.72f,0.32f,0.28f); glVertex2f(cx, cy);
    for (int i = 0; i <= 120; ++i) {
        float a = PI * i / 120.f;
        col(mixc(Col{0.58f,0.22f,0.20f}, Col{0.84f,0.42f,0.36f}, sinf(a)));
        glVertex2f(cx + 365.f*cosf(a), cy + 365.f*sinf(a));
    }
    glEnd();
    glLineWidth(3.f); col(0.42f,0.14f,0.12f);
    for (int k = 0; k < 2; ++k) {
        float r = k ? 300.f : 335.f;
        glBegin(GL_LINE_STRIP); for (int i = 0; i <= 90; ++i) { float a = PI*i/90.f; glVertex2f(cx + r*cosf(a), cy + r*sinf(a)); } glEnd();
    }
    archWindow(420.f, 415.f, 24.f, 34.f); archWindow(480.f, 415.f, 24.f, 34.f);
    archWindow(846.f, 415.f, 24.f, 34.f); archWindow(906.f, 415.f, 24.f, 34.f);
    drawClock(675.f, 566.f, 26.f);
    // name board
    const char* txt = "RAILWAY STATION";
    raw(mixc(lit(Col{0.67f,0.60f,0.90f}), Col{0.10f,0.10f,0.32f}, st.night)); rect(550.f, 450.f, 770.f, 510.f);
    raw(mixc(lit(Col{1.f,1.f,1.f}), Col{0.9f,0.85f,0.4f}, st.night)); glLineWidth(2.f);
    glBegin(GL_LINE_LOOP); glVertex2f(553,453); glVertex2f(767,453); glVertex2f(767,507); glVertex2f(553,507); glEnd();
    raw(mixc(Col{0.f,0.f,0.f}, Col{1.f,1.f,0.6f}, st.night));
    strokeCentered(txt, 660.f, 480.f, 190.f);
    glow(660.f, 480.f, 170.f, Col{1.f,0.9f,0.5f}, 0.18f * st.night);
}

// ------------------------------------------------------------ rail & platform
static void drawTrack(float nearY, float farY) {
    col(0.40f,0.38f,0.36f); rect(0, nearY - 6.f, SW, farY + 6.f);
    col(0.26f,0.17f,0.10f);
    glBegin(GL_QUADS);
    for (float x = -4.f; x < SW + 10.f; x += 24.f) {
        glVertex2f(x, nearY-4.f); glVertex2f(x+9.f, nearY-4.f); glVertex2f(x+9.f, farY+4.f); glVertex2f(x, farY+4.f);
    }
    glEnd();
    float ys[2] = { nearY, farY };
    for (int i = 0; i < 2; ++i) {
        float y = ys[i];
        col(0.20f,0.20f,0.22f); rect(0, y-3.5f, SW, y-1.5f);
        col(0.62f,0.64f,0.68f); rect(0, y-2.f,  SW, y+2.f);
        col(0.92f,0.94f,0.97f); rect(0, y+1.f,  SW, y+2.2f);
    }
}

static void drawRailBed() {
    vrect(0, 0, SW, 250, Col{0.46f,0.44f,0.42f}, Col{0.62f,0.60f,0.58f});
    glPointSize(2.f);
    glBegin(GL_POINTS);
    for (int i = 0; i < NGRAVEL; ++i) { float g = 0.30f + 0.25f * hash1((float)i); col(g, g*0.98f, g*0.95f, 0.55f); glVertex2f(gravX[i], gravY[i]); }
    glEnd();
    drawTrack(28.f, 88.f);
    drawTrack(118.f, 178.f);
}

static void drawPlatform() {
    vrect(300, 205, 1050, 250, Col{0.30f,0.29f,0.28f}, Col{0.55f,0.53f,0.50f});
    col(0.12f,0.12f,0.13f); rect(300, 205, 1050, 213);
    glBegin(GL_QUADS);
    col(0.60f,0.55f,0.50f); glVertex2f(300,250); glVertex2f(1050,250);
    col(0.50f,0.50f,0.52f); glVertex2f(1053,270); glVertex2f(303,270);
    glEnd();
    col(0.96f,0.80f,0.10f); rect(300, 251, 1050, 255);                       // safety line
    for (int x = 350; x <= 1020; x += 100) {                                 // pillars
        glBegin(GL_QUADS);
        col(0.46f,0.37f,0.30f); glVertex2f((float)x,270); glVertex2f(x+10.f,270); glVertex2f(x+10.f,330); glVertex2f(x-30.f,330);
        col(0.36f,0.28f,0.22f); glVertex2f(x+10.f,270); glVertex2f(x+20.f,270); glVertex2f(x+30.f,330); glVertex2f(x+10.f,330);
        glEnd();
    }
    glBegin(GL_QUADS);                                                       // roof
    col(0.02f,0.45f,0.72f); glVertex2f(260,330); glVertex2f(1080,330);
    col(0.20f,0.70f,0.92f); glVertex2f(1050,400); glVertex2f(300,400);
    glEnd();
    col(0.90f,0.92f,0.95f); rect(260, 326, 1080, 332);
    col(0.02f,0.30f,0.55f); rect(300, 396, 1050, 400);
}

static void bench(float x) {
    col(0.35f,0.20f,0.10f);
    rect(x-24, 258, x+24, 262); rect(x-24, 265, x+24, 275);
    col(0.10f,0.10f,0.12f); rect(x-22, 252, x-19, 258); rect(x+19, 252, x+22, 258);
}

static void drawPeople() {
    bench(520.f); bench(830.f);
    for (int i = 0; i < NPERSON; ++i) {
        const Person& p = ppl[i];
        if (p.aboard) continue;
        float walk = (p.st == P_WAIT) ? 0.f : 1.f;
        float gait = walk * sinf(st.simTime * 9.f + p.ph);
        float y = 252.f + sinf(st.simTime * 2.f + p.ph) * 0.5f - walk * fabsf(gait) * 1.4f, h = p.h;
        shadow(p.x, 252.f, 34.f * h, 6.f);
        col(0.12f,0.14f,0.25f);
        rect(p.x-5-2.f*gait, y, p.x-0.5f-2.f*gait, y+13*h);
        rect(p.x+0.5f+2.f*gait, y, p.x+5+2.f*gait, y+13*h);
        col(p.shirt);           rect(p.x-6, y+13*h, p.x+6, y+27*h);
        col(scalec(p.shirt, 0.75f));
        rect(p.x-8+2.f*gait, y+14*h, p.x-6+2.f*gait, y+25*h);
        rect(p.x+6-2.f*gait, y+14*h, p.x+8-2.f*gait, y+25*h);
        col(0.87f,0.69f,0.55f); disc(p.x, y+32*h, 5.2f, 14);
        col(0.10f,0.07f,0.05f); halfDisc(p.x, y+33*h, 5.4f, 10);
    }
}

static void streetLamp(float xp, float dir) {
    col(0.05f,0.05f,0.06f); rect(xp-2.5f, 270, xp+2.5f, 360);
    glBegin(GL_QUADS); glVertex2f(xp,354); glVertex2f(xp+dir*30,350); glVertex2f(xp+dir*30,353); glVertex2f(xp,358); glEnd();
    float bx = xp + dir * 34.f, by = 346.f;
    raw(mixc(lit(Col{0.92f,0.92f,0.88f}), Col{1.f,0.95f,0.55f}, st.night)); disc(bx, by, 7.f, 16);
    Col warm{1.f,0.88f,0.5f};
    lightCone(bx, by, 70.f, 270.f, 0.30f * st.night, warm);
    glow(bx, by, 95.f, warm, 0.55f * st.night);
}
static void hangLamp(float x) {
    col(0.1f,0.1f,0.1f); rect(x-0.6f, 320, x+0.6f, 330);
    raw(mixc(lit(Col{0.9f,0.9f,0.85f}), Col{1.f,0.95f,0.6f}, st.night)); disc(x, 317.f, 4.5f, 12);
    Col warm{1.f,0.88f,0.5f};
    lightCone(x, 317.f, 45.f, 270.f, 0.20f * st.night, warm);
    glow(x, 317.f, 55.f, warm, 0.40f * st.night);
}

static void drawFence(float xa, float xb) {
    col(0.50f,0.22f,0.14f); rect(xa, 262, xb, 266); rect(xa, 292, xb, 296);
    for (float x = xa; x < xb; x += 16.f) {
        float k = 0.9f + 0.2f * hash1(x);
        col(0.70f*k, 0.30f*k, 0.18f*k);
        glBegin(GL_QUADS); glVertex2f(x,240); glVertex2f(x+8,240); glVertex2f(x+8,318); glVertex2f(x,318); glEnd();
        glBegin(GL_TRIANGLES); glVertex2f(x,318); glVertex2f(x+8,318); glVertex2f(x+4,326); glEnd();
    }
}

static void drawSignal() {
    float x = 1172.f, y0 = 250.f;
    col(0.25f,0.25f,0.27f); rect(x-2, y0, x+2, y0+84);
    col(0.06f,0.06f,0.07f); rect(x-9, y0+70, x+9, y0+112);
    bool red = (t1.state == BRAKE || t1.state == STOPPED);
    Col r = red ? Col{1.f,0.10f,0.05f} : Col{0.25f,0.05f,0.04f};
    Col g = red ? Col{0.04f,0.20f,0.06f} : Col{0.10f,1.f,0.30f};
    raw(r); disc(x, y0+101.f, 5.5f, 14);
    raw(g); disc(x, y0+81.f,  5.5f, 14);
    glow(x, red ? y0+101.f : y0+81.f, 36.f, red ? Col{1.f,0.15f,0.05f} : Col{0.2f,1.f,0.4f}, 0.18f + 0.5f * st.night);
}

// ---------------------------------------------------------------------- trains
static void wheel(float cx, float cy, float rot) {
    glPushMatrix(); glTranslatef(cx, cy, 0.f); glRotatef(rot * 180.f / PI, 0.f, 0.f, 1.f);
    col(0.08f,0.08f,0.09f); disc(0, 0, 16.f, 26);
    col(0.58f,0.58f,0.62f); disc(0, 0, 11.f, 22);
    col(0.20f,0.20f,0.22f); glLineWidth(2.f);
    glBegin(GL_LINES);
    for (int k = 0; k < 5; ++k) { float a = k * 2.f * PI / 5.f; glVertex2f(0, 0); glVertex2f(11.f*cosf(a), 11.f*sinf(a)); }
    glEnd();
    col(0.85f,0.2f,0.1f); disc(7.f, 0.f, 1.8f, 8);                     // crank pin: makes rotation visible
    col(0.40f,0.40f,0.43f); disc(0, 0, 3.5f, 10);
    glPopMatrix();
}

static Col glassCol() { return mixc(lit(Col{0.60f,0.80f,0.92f}), Col{1.f,0.86f,0.48f}, st.night); }

static void drawTrain(const Train& t, Col liv) {
    float v01 = clampf(t.v / t.vmax, 0.f, 1.f);
    float shake = sinf(st.simTime * 55.f + t.wheelY) * 0.6f * v01;
    float y0 = t.wheelY - 3.f, nx = N_CARS * (CAR_W + GAP) - GAP;
    glColor4f(0.f,0.f,0.f,0.28f); rect(t.x + 4.f, t.wheelY - 19.f, t.x + TRAIN_LEN - 4.f, t.wheelY - 13.f);
    glPushMatrix();
    glTranslatef(t.dir > 0 ? t.x : t.x + TRAIN_LEN, shake, 0.f);
    glScalef(t.dir, 1.f, 1.f);                                          // local +x = driving direction
    Col lo = scalec(liv, 0.65f), hi = scalec(liv, 1.15f), gl = glassCol();
    static const float wx[3] = {8.f, 38.f, 92.f}, ww[3] = {24.f, 24.f, 22.f};
    for (int i = 0; i < N_CARS; ++i) {
        float cx = i * (CAR_W + GAP);
        if (i < N_CARS - 1) {
            col(0.10f,0.10f,0.12f); rect(cx+CAR_W-1, y0+14, cx+CAR_W+GAP+1, y0+72);
            col(0.35f,0.35f,0.38f); rect(cx+CAR_W,   y0+10, cx+CAR_W+GAP,   y0+14);
        }
        col(0.12f,0.12f,0.14f); rect(cx+6, y0-1, cx+CAR_W-6, y0+9);
        vrect(cx, y0+6, cx+CAR_W, y0+85, lo, hi);
        col(0.98f,0.85f,0.15f); rect(cx, y0+27, cx+CAR_W, y0+32);
        col(0.75f,0.12f,0.10f); rect(cx-1, y0+85, cx+CAR_W+1, y0+90);
        col(0.55f,0.57f,0.60f); rect(cx+25, y0+90, cx+50, y0+95); rect(cx+70, y0+90, cx+95, y0+95);
        for (int k = 0; k < 3; ++k) {
            col(0.08f,0.08f,0.10f); rect(cx+wx[k]-1.5f, y0+39.5f, cx+wx[k]+ww[k]+1.5f, y0+72.5f);
            raw(gl);                rect(cx+wx[k],      y0+41.f,  cx+wx[k]+ww[k],      y0+71.f);
            float pxm = cx + wx[k] + ww[k] * 0.5f;
            if (hash1(t.wheelY + i * 7.f + k) > 0.35f) {                      // passenger silhouette
                glColor4f(0.10f, 0.09f, 0.10f, 0.6f + 0.3f * st.night);
                disc(pxm, y0 + 62.f, 4.5f, 12); rect(pxm - 7.f, y0 + 41.f, pxm + 7.f, y0 + 56.f);
            }
            glow(pxm, y0 + 56.f, 38.f, Col{1.f,0.85f,0.5f}, 0.16f * st.night);
        }
        col(0.10f,0.10f,0.12f); rect(cx+67, y0+7, cx+87, y0+75);        // door
        raw(mixc(lit(Col{0.16f,0.17f,0.20f}), Col{1.f,0.82f,0.45f}, st.night * 0.9f)); rect(cx+68, y0+8, cx+86, y0+74);
        float pw = 18.f * (1.f - t.door * 0.95f);
        if (pw > 0.5f) {
            vrect(cx+68, y0+8, cx+68+pw, y0+74, scalec(liv, 0.6f), scalec(liv, 1.0f));
            if (pw > 10.f) { raw(gl); rect(cx+71, y0+44, cx+68+pw-3.f, y0+68); }
        }
        float wxs[4] = {16.f, 40.f, 80.f, 104.f};
        for (int w = 0; w < 4; ++w) wheel(cx + wxs[w], t.wheelY, -t.dir * t.x / 16.f);
    }
    col(0.95f,0.80f,0.12f);                                              // cab nose
    glBegin(GL_QUADS); glVertex2f(nx,y0+6); glVertex2f(nx+16,y0+6); glVertex2f(nx+12,y0+38); glVertex2f(nx,y0+70); glEnd();
    raw(mixc(lit(Col{0.95f,0.95f,0.85f}), Col{1.f,1.f,0.85f}, st.night)); disc(nx + 11.f, y0 + 24.f, 4.5f, 14);
    raw(mixc(lit(Col{0.5f,0.1f,0.1f}),    Col{1.f,0.15f,0.10f}, st.night)); disc(3.f, y0 + 24.f, 3.5f, 12);
    if (st.night > 0.02f) {
        float a = 0.5f * st.night * (1.f - 0.3f * st.overcast);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBegin(GL_TRIANGLES);
        glColor4f(1.f,0.95f,0.7f,a);   glVertex2f(nx+14.f, y0+24.f);
        glColor4f(1.f,0.95f,0.7f,0.f); glVertex2f(nx+334.f, y0-26.f); glVertex2f(nx+334.f, y0+74.f);
        glEnd();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glow(nx + 14.f, y0 + 24.f, 40.f, Col{1.f,0.95f,0.7f}, 0.6f * st.night);
        glow(3.f, y0 + 24.f, 24.f, Col{1.f,0.1f,0.05f}, 0.45f * st.night);
    }
    glPopMatrix();
}

// ----------------------------------------------------------- weather / overlay
static void drawRain() {
    int n = (int)(st.overcast * NDROP);
    if (n <= 0) return;
    Col rc = mixc(lit(Col{0.85f,0.90f,1.f}), Col{0.5f,0.55f,0.7f}, st.night * 0.5f);
    glLineWidth(1.2f);
    glBegin(GL_LINES);
    for (int i = 0; i < n; ++i) {
        glColor4f(rc.r, rc.g, rc.b, 0.35f);
        glVertex2f(drops[i].x, drops[i].y); glVertex2f(drops[i].x + 0.18f*drops[i].len, drops[i].y + drops[i].len);
    }
    glEnd();
    float f = st.flash * st.flash * 0.5f;
    if (f > 0.01f) { glColor4f(0.85f,0.9f,1.f,f); rect(0, 0, SW, SH); }
}

static void vignette(float a) {
    float e = 170.f;
    glBegin(GL_QUADS);
    glColor4f(0,0,0,a); glVertex2f(0,0);   glColor4f(0,0,0,0); glVertex2f(e,0);    glVertex2f(e,SH);    glColor4f(0,0,0,a); glVertex2f(0,SH);
    glColor4f(0,0,0,a); glVertex2f(SW,0);  glColor4f(0,0,0,0); glVertex2f(SW-e,0);  glVertex2f(SW-e,SH); glColor4f(0,0,0,a); glVertex2f(SW,SH);
    glColor4f(0,0,0,a); glVertex2f(0,0);   glVertex2f(SW,0);   glColor4f(0,0,0,0); glVertex2f(SW,e);     glVertex2f(0,e);
    glColor4f(0,0,0,a); glVertex2f(0,SH);  glVertex2f(SW,SH);  glColor4f(0,0,0,0); glVertex2f(SW,SH-e);  glVertex2f(0,SH-e);
    glEnd();
}

static void textAt(float x, float y, const char* s, void* font) {
    glRasterPos2f(x, y);
    for (; *s; ++s) glutBitmapCharacter(font, *s);
}

static void drawHUD() {
    glViewport(0, 0, winW, winH);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); gluOrtho2D(0, winW, 0, winH);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glColor4f(0.f,0.f,0.f,0.45f); rect(8, (float)winH - 92, 540, (float)winH - 8);
    glColor4f(1.f,1.f,1.f,0.95f);
    char b[160];
    int hh = (int)st.tod, mm = (int)((st.tod - hh) * 60.f), onb = 0;
    for (int i = 0; i < NPERSON; ++i) if (ppl[i].aboard) ++onb;
    snprintf(b, sizeof b, "RailView   %02d:%02d   %s   %.0f fps   on board: %d", hh, mm, st.paused ? "PAUSED" : "running", st.fps, onb);
    textAt(16, (float)winH - 26, b, GLUT_BITMAP_HELVETICA_12);
    snprintf(b, sizeof b, "[D] day  [N] night  [A] auto cycle: %s  [R] rain: %s", st.autoCycle ? "on" : "off", st.rain ? "on" : "off");
    textAt(16, (float)winH - 44, b, GLUT_BITMAP_HELVETICA_12);
    snprintf(b, sizeof b, "[SPACE] pause  [+/-] speed x%.2f  [S] sound: %s  [B] bloom: %s",
             st.timeScale, st.audio ? "on" : "off", (post.avail && st.post) ? "on" : "off");
    textAt(16, (float)winH - 62, b, GLUT_BITMAP_HELVETICA_12);
    snprintf(b, sizeof b, "[H] HUD  [F] fullscreen  [ESC] quit");
    textAt(16, (float)winH - 80, b, GLUT_BITMAP_HELVETICA_12);
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
}

// ---------------------------------------------------------- post: bloom/grade
typedef GLuint (APIENTRY *PFN_createshader)(GLenum);
typedef void   (APIENTRY *PFN_shadersource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void   (APIENTRY *PFN_compileshader)(GLuint);
typedef void   (APIENTRY *PFN_getshaderiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFN_getshaderlog)(GLuint, GLsizei, GLsizei*, char*);
typedef GLuint (APIENTRY *PFN_createprogram)(void);
typedef void   (APIENTRY *PFN_attachshader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_linkprogram)(GLuint);
typedef void   (APIENTRY *PFN_getprogramiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFN_getprogramlog)(GLuint, GLsizei, GLsizei*, char*);
typedef void   (APIENTRY *PFN_useprogram)(GLuint);
typedef GLint  (APIENTRY *PFN_getuniformlocation)(GLuint, const char*);
typedef void   (APIENTRY *PFN_uniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_uniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_uniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_genfb)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFN_bindfb)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_deletefb)(GLsizei, const GLuint*);
typedef GLenum (APIENTRY *PFN_checkfb)(GLenum);
typedef void   (APIENTRY *PFN_fbtex2d)(GLenum, GLenum, GLenum, GLuint, GLint);

static void* glProc(const char* n) {
#ifdef _WIN32
    return (void*)wglGetProcAddress(n);
#elif defined(__APPLE__)
    return dlsym(RTLD_DEFAULT, n);
#else
    return (void*)glXGetProcAddressARB((const GLubyte*)n);
#endif
}
#define LOADP(var, name) do { var = (decltype(var))glProc(name); } while (0)
#define LOADP2(var, name, alt) do { var = (decltype(var))glProc(name); if (!var) var = (decltype(var))glProc(alt); } while (0)

static PFN_createshader        pCreateShader;
static PFN_shadersource        pShaderSource;
static PFN_compileshader       pCompileShader;
static PFN_getshaderiv         pGetShaderiv;
static PFN_getshaderlog        pGetShaderLog;
static PFN_createprogram       pCreateProgram;
static PFN_attachshader        pAttachShader;
static PFN_linkprogram         pLinkProgram;
static PFN_getprogramiv        pGetProgramiv;
static PFN_getprogramlog       pGetProgramLog;
static PFN_useprogram          pUseProgram;
static PFN_getuniformlocation  pGetUniformLoc;
static PFN_uniform1f           pUniform1f;
static PFN_uniform2f           pUniform2f;
static PFN_uniform1i           pUniform1i;
static PFN_genfb               pGenFB;
static PFN_bindfb              pBindFB;
static PFN_deletefb            pDeleteFB;
static PFN_checkfb             pCheckFB;
static PFN_fbtex2d             pFBTex2D;

static const char* VS_QUAD =
    "void main() {\n"
    "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char* FS_BRIGHT =
    "uniform sampler2D tex;\n"
    "uniform float threshold;\n"
    "void main() {\n"
    "  vec3 c = texture2D(tex, gl_TexCoord[0].st).rgb;\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  float k = max(l - threshold, 0.0) / max(l, 0.0001);\n"
    "  gl_FragColor = vec4(c * k, 1.0);\n"
    "}\n";

static const char* FS_BLUR =
    "uniform sampler2D tex;\n"
    "uniform vec2 dir;\n"
    "void main() {\n"
    "  vec2 t = gl_TexCoord[0].st;\n"
    "  vec4 s = texture2D(tex, t) * 0.2270270270;\n"
    "  s += (texture2D(tex, t + dir * 1.3846153846) + texture2D(tex, t - dir * 1.3846153846)) * 0.3162162162;\n"
    "  s += (texture2D(tex, t + dir * 3.2307692308) + texture2D(tex, t - dir * 3.2307692308)) * 0.0702702703;\n"
    "  gl_FragColor = s;\n"
    "}\n";

static const char* FS_GRADE =
    "uniform sampler2D tex;\n"
    "uniform float exposure, contrast, saturation;\n"
    "void main() {\n"
    "  vec3 c = texture2D(tex, gl_TexCoord[0].st).rgb;\n"
    "  c = vec3(1.0) - exp(-c * exposure);\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  c = mix(vec3(l), c, saturation);\n"
    "  c = (c - 0.5) * contrast + 0.5;\n"
    "  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

static const char* FS_BLOOM =
    "uniform sampler2D tex;\n"
    "uniform float strength;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(texture2D(tex, gl_TexCoord[0].st).rgb * strength, 1.0);\n"
    "}\n";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = pCreateShader(type);
    pShaderSource(s, 1, &src, nullptr);
    pCompileShader(s);
    GLint ok = 0; pGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        pGetShaderLog(s, sizeof log, &n, log);
        printf("shader error: %.*s\n", (int)n, log);
        return 0;
    }
    return s;
}

static GLuint linkShaders(GLuint vs, GLuint fs) {
    GLuint p = pCreateProgram();
    pAttachShader(p, vs); pAttachShader(p, fs);
    pLinkProgram(p);
    GLint ok = 0; pGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        pGetProgramLog(p, sizeof log, &n, log);
        printf("link error: %.*s\n", (int)n, log);
        return 0;
    }
    return p;
}

static GLuint mkTex(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return t;
}

static void postResize();

static void postInit() {
    if (!USE_POST) return;
    LOADP(pCreateShader,       "glCreateShader");
    LOADP(pShaderSource,       "glShaderSource");
    LOADP(pCompileShader,      "glCompileShader");
    LOADP(pGetShaderiv,        "glGetShaderiv");
    LOADP(pGetShaderLog,       "glGetShaderInfoLog");
    LOADP(pCreateProgram,      "glCreateProgram");
    LOADP(pAttachShader,       "glAttachShader");
    LOADP(pLinkProgram,        "glLinkProgram");
    LOADP(pGetProgramiv,       "glGetProgramiv");
    LOADP(pGetProgramLog,      "glGetProgramInfoLog");
    LOADP(pUseProgram,         "glUseProgram");
    LOADP(pGetUniformLoc,      "glGetUniformLocation");
    LOADP(pUniform1f,          "glUniform1f");
    LOADP(pUniform2f,          "glUniform2f");
    LOADP(pUniform1i,          "glUniform1i");
    LOADP2(pGenFB,             "glGenFramebuffersEXT",         "glGenFramebuffers");
    LOADP2(pBindFB,            "glBindFramebufferEXT",         "glBindFramebuffer");
    LOADP2(pDeleteFB,          "glDeleteFramebuffersEXT",      "glDeleteFramebuffers");
    LOADP2(pCheckFB,           "glCheckFramebufferStatusEXT",  "glCheckFramebufferStatus");
    LOADP2(pFBTex2D,           "glFramebufferTexture2DEXT",    "glFramebufferTexture2D");

    if (!pCreateShader || !pCreateProgram || !pGenFB || !pBindFB || !pFBTex2D || !pCheckFB || !pGetUniformLoc) {
        printf("post: GL 2.0 / FBO not available - bloom disabled\n");
        return;
    }
    GLuint vs = compileShader(GL_VERTEX_SHADER, VS_QUAD);
    if (!vs) return;
    post.pBright = linkShaders(vs, compileShader(GL_FRAGMENT_SHADER, FS_BRIGHT));
    post.pBlur   = linkShaders(vs, compileShader(GL_FRAGMENT_SHADER, FS_BLUR));
    post.pGrade  = linkShaders(vs, compileShader(GL_FRAGMENT_SHADER, FS_GRADE));
    post.pBloom  = linkShaders(vs, compileShader(GL_FRAGMENT_SHADER, FS_BLOOM));
    if (!post.pBright || !post.pBlur || !post.pGrade || !post.pBloom) return;

    pGenFB(1, &post.fboS); pGenFB(1, &post.fboA); pGenFB(1, &post.fboB);
    post.avail = true;
    post.sw = post.sh = 0;
    postResize();
    printf("post: bloom + colour grade active (%.1fx scene buffer)\n", (double)SSAA);
}

static void postResize() {
    if (!post.avail) return;
    int w = (int)(vpW * SSAA), h = (int)(vpH * SSAA);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w == post.sw && h == post.sh) return;
    post.sw = w; post.sh = h;
    post.aw = (w + 1) / 2; post.ah = (h + 1) / 2;
    if (post.texS) glDeleteTextures(1, &post.texS);
    if (post.texA) glDeleteTextures(1, &post.texA);
    if (post.texB) glDeleteTextures(1, &post.texB);
    post.texS = mkTex(post.sw, post.sh);
    post.texA = mkTex(post.aw, post.ah);
    post.texB = mkTex(post.aw, post.ah);
    GLuint fbos[3] = { post.fboS, post.fboA, post.fboB };
    GLuint texs[3] = { post.texS, post.texA, post.texB };
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        pBindFB(GL_FRAMEBUFFER_EXT, fbos[i]);
        pFBTex2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texs[i], 0);
        if (pCheckFB(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) ok = false;
    }
    pBindFB(GL_FRAMEBUFFER_EXT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!ok) { printf("post: framebuffer incomplete - bloom disabled\n"); post.avail = false; }
}

static void quadTC(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y0);
    glTexCoord2f(1, 1); glVertex2f(x1, y1);
    glTexCoord2f(0, 1); glVertex2f(x0, y1);
    glEnd();
}

static void postBegin() {
    pBindFB(GL_FRAMEBUFFER_EXT, post.fboS);
    glViewport(0, 0, post.sw, post.sh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluOrtho2D(0.0, SW, 0.0, SH);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
}

static void postEnd() {
    glDisable(GL_BLEND);
    // 1. bright pass -> texA (half res)
    pBindFB(GL_FRAMEBUFFER_EXT, post.fboA);
    glViewport(0, 0, post.aw, post.ah);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluOrtho2D(0.0, 1.0, 0.0, 1.0);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    pUseProgram(post.pBright);
    pUniform1i(pGetUniformLoc(post.pBright, "tex"), 0);
    pUniform1f(pGetUniformLoc(post.pBright, "threshold"), lerpf(0.86f, 0.40f, st.night));
    glBindTexture(GL_TEXTURE_2D, post.texS);
    quadTC(0.f, 0.f, 1.f, 1.f);
    // 2. separable blur, two iterations (second one wider)
    for (int it = 0; it < 4; ++it) {
        bool toB = ((it & 1) == 0);
        pBindFB(GL_FRAMEBUFFER_EXT, toB ? post.fboB : post.fboA);
        pUseProgram(post.pBlur);
        pUniform1i(pGetUniformLoc(post.pBlur, "tex"), 0);
        float sc = (it < 2) ? 1.f : 2.f;
        if (toB) pUniform2f(pGetUniformLoc(post.pBlur, "dir"), sc / post.aw, 0.f);
        else     pUniform2f(pGetUniformLoc(post.pBlur, "dir"), 0.f, sc / post.ah);
        glBindTexture(GL_TEXTURE_2D, toB ? post.texA : post.texB);
        quadTC(0.f, 0.f, 1.f, 1.f);
    }
    // 3. grade the scene into the window, then add the bloom on top
    pBindFB(GL_FRAMEBUFFER_EXT, 0);
    glViewport(0, 0, winW, winH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluOrtho2D(0.0, winW, 0.0, winH);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    pUseProgram(post.pGrade);
    pUniform1i(pGetUniformLoc(post.pGrade, "tex"), 0);
    pUniform1f(pGetUniformLoc(post.pGrade, "exposure"),   1.06f);
    pUniform1f(pGetUniformLoc(post.pGrade, "contrast"),   1.05f);
    pUniform1f(pGetUniformLoc(post.pGrade, "saturation"), 1.08f);
    glBindTexture(GL_TEXTURE_2D, post.texS);
    quadTC((float)vpX, (float)vpY, (float)(vpX + vpW), (float)(vpY + vpH));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    pUseProgram(post.pBloom);
    pUniform1i(pGetUniformLoc(post.pBloom, "tex"), 0);
    pUniform1f(pGetUniformLoc(post.pBloom, "strength"), lerpf(0.55f, 0.95f, st.night));
    glBindTexture(GL_TEXTURE_2D, post.texA);
    quadTC((float)vpX, (float)vpY, (float)(vpX + vpW), (float)(vpY + vpH));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ------------------------------------------------------------------- sound
#if USE_SOUND && defined(_WIN32)

static const int AUD_SR   = 22050;
static const int AUD_LOOP = AUD_SR * 2;          // 2 s seamless loops
static const int AUD_HORN = (int)(AUD_SR * 1.45f);
static const int AUD_THUN = AUD_SR * 2;
static const int AUD_BUF  = AUD_SR / 5;          // 0.2 s chunks
static const int AUD_NBUF = 4;

static short sndRain[AUD_LOOP], sndRumb[AUD_LOOP], sndHorn[AUD_HORN], sndThun[AUD_THUN];
static volatile float aRain = 0.f, aRumb = 0.f;
static volatile int   aHornPos = -1, aThunPos = -1, aThunDelay = -1;
static bool  audReady = false;
static int   audIdx = 0;

static HWAVEOUT audWo = 0;
static short    audBuf[AUD_NBUF][AUD_BUF];
static WAVEHDR  audHdr[AUD_NBUF];

// every partial sits on a 0.5 Hz grid, so the 2 s loop wraps without a click
static void synthLoops() {
    {
        static float acc[AUD_LOOP];
        memset(acc, 0, sizeof acc);
        for (int k = 0; k < 150; ++k) {                                  // rain hiss
            float f  = floorf((110.f + k * 52.f) * 2.f + 0.5f) * 0.5f;
            float am = 1.f / (1.f + f / 750.f), ph = frand() * 2.f * PI;
            for (int i = 0; i < AUD_LOOP; ++i) acc[i] += am * sinf(2.f * PI * f * i / AUD_SR + ph);
        }
        float mx = 1e-6f;
        for (int i = 0; i < AUD_LOOP; ++i) mx = fmaxf(mx, fabsf(acc[i]));
        for (int i = 0; i < AUD_LOOP; ++i) sndRain[i] = (short)(acc[i] / mx * 23000.f);
    }
    {
        static float acc[AUD_LOOP];
        memset(acc, 0, sizeof acc);
        static const float fr[15] = {16,19,23,27,31,37,43,49,57,67,79,91,107,127,151};
        for (int k = 0; k < 15; ++k) {                                   // wheel/track rumble
            float am = 1.f / (1.f + fr[k] / 45.f), ph = frand() * 2.f * PI;
            for (int i = 0; i < AUD_LOOP; ++i) acc[i] += am * sinf(2.f * PI * fr[k] * i / AUD_SR + ph);
        }
        float mx = 1e-6f;
        for (int i = 0; i < AUD_LOOP; ++i) mx = fmaxf(mx, fabsf(acc[i]));
        for (int i = 0; i < AUD_LOOP; ++i) {
            float w = acc[i] / mx * (0.72f + 0.28f * sinf(2.f * PI * 0.5f * i / AUD_SR));
            sndRumb[i] = (short)(w * 26000.f);
        }
    }
}

static void synthOneShots() {
    const float dur = 1.4f, atk = 0.05f, rel = 0.30f;                   // air horn
    for (int i = 0; i < AUD_HORN; ++i) {
        float t = i / (float)AUD_SR, e = 1.f;
        if (t < atk) e = t / atk;
        else if (t > dur - rel) e = (dur - t) / rel;
        e = clampf(e, 0.f, 1.f); e = e * e * (3.f - 2.f * e);
        float w = 0.60f * sinf(2.f * PI * 233.08f * t + 0.9f * sinf(2.f * PI * 5.f * t))
                + 0.45f * sinf(2.f * PI * 349.23f * t)
                + 0.16f * sinf(2.f * PI * 466.16f * t);
        sndHorn[i] = (short)(w * e * 21000.f);
    }
    unsigned seed = 99991u; float lp = 0.f;                             // thunder
    for (int i = 0; i < AUD_THUN; ++i) {
        float t = i / (float)AUD_SR;
        seed = seed * 1664525u + 1013904223u;
        float x = ((seed >> 16) & 0x7fff) / 16384.f - 1.f;
        lp += (x - lp) * clampf(0.55f * expf(-2.2f * t) + 0.012f, 0.005f, 0.6f);
        float env   = expf(-1.7f * t) * (1.f - expf(-40.f * t));
        float thump = sinf(2.f * PI * (48.f - 12.f * t) * t) * expf(-3.f * t) * 0.75f;
        sndThun[i] = (short)(clampf(lp * env * 1.15f + thump, -1.f, 1.f) * 30000.f);
    }
}

static void audFill(short* out, int n) {
    for (int i = 0; i < n; ++i) {
        int li = audIdx % AUD_LOOP;
        float s = sndRain[li] * (aRain * 0.55f) + sndRumb[li] * (aRumb * 0.60f);
        if (aThunDelay > 0) --aThunDelay;
        else if (aThunPos >= 0) { s += sndThun[aThunPos] * 0.85f; if (++aThunPos >= AUD_THUN) aThunPos = -1; }
        if (aHornPos >= 0)      { s += sndHorn[aHornPos] * 0.75f; if (++aHornPos >= AUD_HORN) aHornPos = -1; }
        ++audIdx;
        out[i] = (short)clampf(s, -32000.f, 32000.f);
    }
}

static void CALLBACK audDone(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    WAVEHDR* h = (WAVEHDR*)p1;
    if (!(h->dwFlags & WHDR_PREPARED)) return;
    audFill((short*)h->lpData, AUD_BUF);
    waveOutWrite(audWo, h, sizeof(WAVEHDR));
}

static bool audStart() {
    WAVEFORMATEX w; memset(&w, 0, sizeof w);
    w.wFormatTag = WAVE_FORMAT_PCM; w.nChannels = 1;
    w.nSamplesPerSec = AUD_SR; w.wBitsPerSample = 16;
    w.nBlockAlign = w.nChannels * w.wBitsPerSample / 8;
    w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
    if (waveOutOpen(&audWo, WAVE_MAPPER, &w, (DWORD_PTR)audDone, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;
    for (int i = 0; i < AUD_NBUF; ++i) {
        memset(audBuf[i], 0, sizeof audBuf[i]);
        memset(&audHdr[i], 0, sizeof audHdr[i]);
        audHdr[i].lpData = (LPSTR)audBuf[i];
        audHdr[i].dwBufferLength = AUD_BUF * sizeof(short);
        waveOutPrepareHeader(audWo, &audHdr[i], sizeof(WAVEHDR));
        audFill(audBuf[i], AUD_BUF);
        waveOutWrite(audWo, &audHdr[i], sizeof(WAVEHDR));
    }
    return true;
}

static void audStop() {
    if (!audWo) return;
    waveOutReset(audWo);
    for (int i = 0; i < AUD_NBUF; ++i) waveOutUnprepareHeader(audWo, &audHdr[i], sizeof(WAVEHDR));
    waveOutClose(audWo); audWo = 0;
}

static void audioStop();

static void audioInit() {
    synthLoops(); synthOneShots();
    audReady = audStart();
    if (audReady) printf("sound: rain / rumble / horn / thunder synthesised (22050 Hz mono)\n");
    else          printf("sound: no audio device available\n");
    atexit(audioStop);
}
static void audioSet(float rain, float rumb, float k) {
    if (!audReady || !st.audio) { aRain = 0.f; aRumb = 0.f; return; }
    aRain += (rain  - aRain) * k;
    aRumb += (rumb  - aRumb) * k;
}
static void audioHorn()    { if (audReady && st.audio) aHornPos = 0; }
static void audioThunder() { if (audReady && st.audio) aThunDelay = AUD_SR / 2; }
static void audioStop()    { audStop(); }

#else

static void audioInit()   { printf("sound: disabled (winmm layer is Windows-only)\n"); st.audio = false; }
static void audioSet(float, float, float) {}
static void audioHorn()    {}
static void audioThunder() {}
static void audioStop()    {}

#endif

// ---------------------------------------------------------------------- frame
static void drawScene() {
    drawSky(); drawStars(); drawMeteor(); drawSun(); drawMoon(); drawClouds(); drawBirds();
    hillLayer(HILL_FAR,  (int)(sizeof(HILL_FAR)  / sizeof(P)), Col{0.40f,0.46f,0.58f}, Col{0.58f,0.64f,0.74f}, 0.45f);
    hillLayer(HILL_NEAR, (int)(sizeof(HILL_NEAR) / sizeof(P)), Col{0.36f,0.30f,0.21f}, Col{0.66f,0.59f,0.46f}, 0.12f);
    drawGround(); drawGrass(); drawRoad();
    for (int i = 0; i < 3; ++i) drawVehicle(cars[i]);
    drawStation();
    drawTree(25.f, 1.00f, 0.f);  drawTree(95.f, 1.10f, 1.3f); drawTree(165.f, 0.95f, 2.1f); drawTree(235.f, 1.05f, 3.4f);
    drawTree(1075.f, 1.05f, 4.2f); drawTree(1135.f, 0.95f, 5.0f);
    drawMist(); drawRailBed(); drawWet();
    drawPlatform(); drawPeople();
    drawFence(0.f, 300.f); drawFence(1060.f, 1200.f);
    streetLamp(300.f, 1.f); streetLamp(1048.f, -1.f);
    hangLamp(475.f); hangLamp(675.f); hangLamp(875.f);
    drawSignal();
    drawTrain(t1, Col{0.10f,0.62f,0.26f});
    drawTrain(t2, Col{0.12f,0.52f,0.58f});
    drawRain();
    vignette(0.12f + 0.25f * st.night);
}

static void display() {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, winW, winH);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (post.avail && st.post && post.sw > 0) {
        postBegin();                                    // scene -> FBO (supersampled)
        drawScene();
        postEnd();                                      // bloom + grade -> window
    } else {
        glViewport(vpX, vpY, vpW, vpH);
        glEnable(GL_SCISSOR_TEST); glScissor(vpX, vpY, vpW, vpH);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluOrtho2D(0.0, SW, 0.0, SH);
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        drawScene();
        glDisable(GL_SCISSOR_TEST);
    }
    if (st.hud) drawHUD();
    glutSwapBuffers();
}

static void reshape(int w, int h) {                       // keep 5:3 aspect, letter-box the rest
    winW = w > 1 ? w : 1; winH = h > 1 ? h : 1;
    float aspect = SW / SH;
    if (winW / (float)winH > aspect) { vpH = winH; vpW = (int)(winH * aspect); }
    else                             { vpW = winW; vpH = (int)(winW / aspect); }
    vpX = (winW - vpW) / 2; vpY = (winH - vpH) / 2;
    postResize();
}

// --------------------------------------------------------------------- update
// -------------------------------------------------------- passenger traffic
static bool doorWasOpen = false;

static float doorWorldX(const Train& t, float nearX) {            // nearest car door, world x
    float best = t.x + 77.f, bd = 1e9f;
    for (int i = 0; i < N_CARS; ++i) {
        float local = i * (CAR_W + GAP) + 77.f;
        float wx = (t.dir > 0.f) ? t.x + local : t.x + TRAIN_LEN - local;
        float d = fabsf(wx - nearX);
        if (d < bd) { bd = d; best = wx; }
    }
    return best;
}

static void updatePassengers(float dt) {
    bool open = (t1.state == STOPPED && t1.door > 0.5f);
    if (open && !doorWasOpen) {
        int queued = 0;
        for (int i = 0; i < NPERSON; ++i) {
            Person& p = ppl[i];
            if (p.aboard) {                                        // alight: step down, walk home
                p.aboard = false;
                p.x = doorWorldX(t1, p.homeX);
                p.st = P_TO_HOME;
            } else if (p.st == P_WAIT && queued < 3 && frand() < 0.75f) {
                p.st = P_TO_DOOR; p.tx = doorWorldX(t1, p.homeX);  // board
                ++queued;
            }
        }
    } else if (!open && doorWasOpen) {
        for (int i = 0; i < NPERSON; ++i)                          // doors shut: they missed it
            if (ppl[i].st == P_TO_DOOR) ppl[i].st = P_TO_HOME;
    }
    doorWasOpen = open;

    for (int i = 0; i < NPERSON; ++i) {
        Person& p = ppl[i];
        if (p.aboard || p.st == P_WAIT) continue;
        float dst = ((p.st == P_TO_DOOR) ? p.tx : p.homeX) - p.x;
        float sp = 62.f * p.h;
        if (fabsf(dst) <= sp * dt) {
            p.x += dst;
            if (p.st == P_TO_DOOR) { p.aboard = true; p.st = P_WAIT; }   // onboard: no longer drawn
            else p.st = P_WAIT;
        } else p.x += ((dst > 0.f) ? sp : -sp) * dt;
    }
}

static void updateTrain(Train& t, float dt) {
    const float A = 150.f;
    float d = (t.stopX - t.x) * t.dir;                  // distance ahead along travel
    switch (t.state) {
    case WAITING:
        t.timer -= dt;
        if (t.timer <= 0.f) { t.state = CRUISE; t.v = t.vmax; t.served = false; }
        break;
    case CRUISE:
        t.x += t.dir * t.v * dt;
        if (t.canStop && !t.served && d > 0.f && d <= t.v * t.v / (2.f * A)) t.state = BRAKE;
        break;
    case BRAKE: {
        d = (t.stopX - t.x) * t.dir;
        if (d <= 0.5f) { t.x = t.stopX; t.v = 0.f; t.state = STOPPED; t.timer = 5.2f; }
        else {
            t.v = clampf(sqrtf(2.f * A * d), 14.f, t.vmax);
            t.x += t.dir * t.v * dt;
            if ((t.stopX - t.x) * t.dir < 0.f) t.x = t.stopX;
        }
        break; }
    case STOPPED:
        t.timer -= dt;
        if (t.timer <= 0.f) { t.state = ACCELERATE; t.served = true; }
        break;
    case ACCELERATE:
        t.v += 110.f * dt;
        if (t.v >= t.vmax) { t.v = t.vmax; t.state = CRUISE; }
        t.x += t.dir * t.v * dt;
        break;
    }
    if (t.state == CRUISE || t.state == ACCELERATE) {
        if (t.dir > 0.f && t.x > SW + 60.f)            { t.state = WAITING; t.timer = 2.f + frand()*4.f; t.x = -TRAIN_LEN - 60.f; t.served = false; }
        if (t.dir < 0.f && t.x < -TRAIN_LEN - 60.f)    { t.state = WAITING; t.timer = 2.f + frand()*4.f; t.x = SW + 60.f;         t.served = false; }
    }
    float tgt = (t.state == STOPPED && t.timer < 4.3f && t.timer > 0.9f) ? 1.f : 0.f;
    t.door += clampf(tgt - t.door, -dt * 1.2f, dt * 1.2f);
}

static void advanceClock(float dt) {
    if (st.todTarget >= 0.f) {
        float d = st.todTarget - st.tod;
        while (d < 0.f) d += 24.f;
        if (d > 23.5f) d = 0.f;
        float step = 7.f * dt;                              // fast-forward: 7 h per second
        if (d <= step) { st.tod = st.todTarget; st.todTarget = -1.f; }
        else st.tod += step;
    } else if (st.autoCycle) st.tod += dt * (24.f / 60.f);  // 60 s per full day
    if (st.tod >= 24.f) st.tod -= 24.f;
}

static void update(float dt) {
    st.fps = 0.95f * st.fps + 0.05f * (1.f / dt);
    float sdt = st.paused ? 0.f : dt * st.timeScale;
    st.simTime += sdt;
    advanceClock(sdt);

    st.overcast += ((st.rain ? 1.f : 0.f) - st.overcast) * clampf(sdt * 0.8f, 0.f, 1.f);
    if (st.rain && st.overcast > 0.6f) {
        st.nextFlash -= sdt;
        if (st.nextFlash <= 0.f) { st.flash = 1.f; st.nextFlash = 4.f + frand() * 9.f; }
    }
    st.flash = fmaxf(0.f, st.flash - sdt * 2.5f);
    updateLighting();

    for (int i = 0; i < NCLOUD; ++i) {
        clouds[i].x -= clouds[i].spd * sdt * (1.f + 0.6f * st.overcast);
        if (clouds[i].x < -130.f * clouds[i].s) clouds[i].x = SW + 130.f * clouds[i].s;
    }
    flockX += 75.f * sdt; if (flockX > 1500.f) flockX = -300.f - frand() * 600.f;
    for (int i = 0; i < NDROP; ++i) {
        drops[i].y -= drops[i].v * sdt; drops[i].x -= drops[i].v * 0.18f * sdt;
        if (drops[i].y < 0.f) { drops[i].y = SH + frand() * 40.f; drops[i].x = frand() * 1400.f - 100.f; }
    }
    for (int i = 0; i < NRIP; ++i) {
        Ripple& r = ripples[i];
        if (r.age < 1.f) r.age += sdt * 1.6f;
        else if (frand() < st.overcast * sdt * 8.f) { r.x = frand() * SW; r.y = frand() * 240.f; r.age = 0.f; }
    }
    if (!met.on) {
        nextMeteor -= sdt;
        if (nextMeteor <= 0.f) {
            if (st.night > 0.9f && st.overcast < 0.3f) {
                met.on = true; met.x = 500.f + frand()*600.f; met.y = 600.f + frand()*100.f;
                met.vx = -(550.f + frand()*300.f); met.vy = -(200.f + frand()*120.f); met.life = 1.f;
                nextMeteor = 7.f + frand() * 10.f;
            } else nextMeteor = 2.f;
        }
    } else {
        met.x += met.vx * sdt; met.y += met.vy * sdt; met.life -= sdt * 1.4f;
        if (met.life <= 0.f) met.on = false;
    }
    updateTrain(t1, sdt); updateTrain(t2, sdt);
    updatePassengers(sdt);
    for (int i = 0; i < 3; ++i) {
        cars[i].x += cars[i].dir * cars[i].speed * sdt;
        if (cars[i].dir > 0.f && cars[i].x > 1260.f) cars[i].x = -60.f;
        if (cars[i].dir < 0.f && cars[i].x < -60.f)  cars[i].x = 1260.f;
    }

    // ---------------------------------------------------------------- audio
    static int   prevSt[2] = { -1, -1 };
    static float prevCx[2] = { -1e9f, -1e9f };
    static float hornCd = 0.f;
    static bool  flashHot = false;
    const Train* tr[2] = { &t1, &t2 };
    for (int i = 0; i < 2; ++i) {
        const Train& t = *tr[i];
        float cx = t.x + TRAIN_LEN * 0.5f;
        bool fire = false;
        if      (prevSt[i] != BRAKE   && t.state == BRAKE)   fire = true;     // horn on approach
        else if (prevSt[i] == STOPPED && t.state == ACCELERATE)   fire = true;     // horn on departure
        else if (t.state == CRUISE && t.v > t.vmax * 0.6f &&
                 (prevCx[i] - 675.f) * (cx - 675.f) < 0.f)   fire = true;     // fast pass through
        if (fire && hornCd <= 0.f) { audioHorn(); hornCd = 1.8f; }
        prevSt[i] = t.state; prevCx[i] = cx;
    }
    hornCd -= dt;
    if (st.flash > 0.85f && !flashHot) audioThunder();
    flashHot = st.flash > 0.85f;

    float rainLvl = st.overcast * 0.9f, rumLvl = 0.f;
    for (int i = 0; i < 2; ++i) {
        const Train& t = *tr[i];
        if (t.x > SW + 260.f || t.x + TRAIN_LEN < -260.f) continue;
        float prox = clampf(1.f - fabsf(t.x + TRAIN_LEN * 0.5f - 675.f) / 1000.f, 0.f, 1.f);
        rumLvl = fmaxf(rumLvl, prox * clampf(t.v / t.vmax, 0.f, 1.f));
    }
    if (st.paused) { rainLvl = 0.f; rumLvl = 0.f; }
    audioSet(rainLvl, rumLvl, clampf(dt * 4.f, 0.f, 1.f));
}

static void tick(int) {
    int now = glutGet(GLUT_ELAPSED_TIME);
    float dt = (now - lastMs) / 1000.f; lastMs = now;
    dt = clampf(dt, 0.001f, 0.05f);
    update(dt);
    glutPostRedisplay();
    glutTimerFunc(16, tick, 0);
}

// ---------------------------------------------------------------------- input
static void keyboard(unsigned char k, int, int) {
    char c = (k >= 'A' && k <= 'Z') ? (char)(k + 32) : (char)k;
    switch (c) {
    case 27:  audioStop(); exit(0);
    case ' ': st.paused = !st.paused; break;
    case '+': case '=': case 107: st.timeScale = clampf(st.timeScale * 1.5f, 0.25f, 6.f); break;
    case '-': case '_': case 111: st.timeScale = clampf(st.timeScale / 1.5f, 0.25f, 6.f); break;
    case 'd': st.autoCycle = false; st.todTarget = 12.f; break;
    case 'n': st.autoCycle = false; st.todTarget = 0.f;  break;
    case 'a': st.autoCycle = !st.autoCycle; st.todTarget = -1.f; break;
    case 'r': st.rain = !st.rain; break;
    case 'h': st.hud = !st.hud; break;
    case 's': st.audio = !st.audio; if (!st.audio) audioSet(0.f, 0.f, 1.f); break;
    case 'b': if (post.avail) st.post = !st.post; break;
    case 'f':
        st.full = !st.full;
        if (st.full) glutFullScreen(); else { glutReshapeWindow(1200, 720); glutPositionWindow(10, 10); }
        break;
    default: break;
    }
}

static void printControls() {
    printf(" RailView controls\n");
    printf("  D  day view            N  night view         A  auto day/night cycle\n");
    printf("  R  rain + lightning    SPACE pause            +/- time speed\n");
    printf("  S  sound on/off        B  bloom on/off        H  toggle HUD\n");
    printf("  F  fullscreen          ESC quit\n");
}

// ----------------------------------------------------------------------- init
static void init() {
    srand(7);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH); glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glEnable(GL_POINT_SMOOTH);
#if USE_MSAA
    glEnable(GL_MULTISAMPLE);
#endif
    for (int i = 0; i < NSTAR; ++i)
        stars[i] = Star{ frand()*SW, 455.f + frand()*265.f, 1.f + frand()*1.8f, frand()*2.f*PI, 1.f + frand()*3.f };
    for (int i = 0; i < NCLOUD; ++i) {
        float s = 0.7f + frand() * 0.9f;
        clouds[i] = Cloud{ frand()*1500.f - 150.f, 540.f + frand()*150.f, s, 8.f + s * 14.f };
    }
    for (int i = 0; i < NDROP; ++i)
        drops[i] = Drop{ frand()*1400.f - 100.f, frand()*SH, 700.f + frand()*300.f, 10.f + frand()*12.f };
    for (int i = 0; i < NGRAVEL; ++i) { gravX[i] = frand() * SW; gravY[i] = frand() * 250.f; }
    for (int i = 0; i < NGRASS; ++i) { grassX[i] = frand() * SW; grassY[i] = 255.f + frand() * 195.f; }
    for (int i = 0; i < NRIP; ++i) ripples[i] = Ripple{ 0.f, 0.f, 2.f };
    static const Col pal[6] = {{0.8f,0.2f,0.2f},{0.2f,0.4f,0.8f},{0.9f,0.7f,0.1f},{0.2f,0.6f,0.3f},{0.6f,0.3f,0.7f},{0.9f,0.5f,0.2f}};
    static const float px[7] = {360.f, 425.f, 560.f, 600.f, 770.f, 880.f, 975.f};
    for (int i = 0; i < NPERSON; ++i) ppl[i] = Person{ px[i], px[i], pal[i % 6], frand()*6.f, 0.9f + frand()*0.2f, P_WAIT, 0.f, false };
    cars[0] = Vehicle{ 100.f, 1.f, 140.f, 44.f, Col{0.80f,0.10f,0.10f} };
    cars[1] = Vehicle{ 900.f,-1.f, 170.f, 50.f, Col{0.10f,0.30f,0.80f} };
    cars[2] = Vehicle{ 500.f, 1.f, 110.f, 40.f, Col{0.90f,0.75f,0.10f} };
    t1 = Train{ -420.f, 330.f, 330.f,  1.f, 134.f, 675.f - TRAIN_LEN * 0.5f, 0.f, 0.f, true,  false, CRUISE };
    t2 = Train{ 1300.f,  430.f, 430.f, -1.f,  44.f, 0.f,                       2.5f, 0.f, false, false, WAITING };
    reshape(1200, 720);
    updateLighting();
    postInit();                                         // GL 2.0 / FBO probe (needs a context)
    audioInit();                                        // synthesises the sound bank
}

int main(int argc, char** argv) {
    glutInit(&argc, argv);
    unsigned int mode = GLUT_DOUBLE | GLUT_RGB;
#if USE_MSAA && defined(GLUT_MULTISAMPLE)
    mode |= GLUT_MULTISAMPLE;
#endif
    glutInitDisplayMode(mode);
    glutInitWindowSize(1200, 720);
    glutInitWindowPosition(10, 10);
    glutCreateWindow("RailView");
    init();
    printControls();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    lastMs = glutGet(GLUT_ELAPSED_TIME);
    glutTimerFunc(16, tick, 0);
    glutMainLoop();
    return 0;
}
