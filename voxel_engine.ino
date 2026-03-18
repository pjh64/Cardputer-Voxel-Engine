/*
 * File: M5Cardputer_VoxelEngine_Improved.ino
 * Description: Refactored Voxel Engine for M5Cardputer.
 * Fixes: Corrected Keyboard access syntax error.
 */

#include <M5Cardputer.h>
#include <cmath>
#include <algorithm>
#include <string.h>

// --- Constants & Macros ---
#define DEFAULT_MAX_FACES 800
#define DEFAULT_MAX_CHUNKS 64
#define MAX_CHUNKS DEFAULT_MAX_CHUNKS
#define MAX_FACES DEFAULT_MAX_FACES
#define SCREEN_W 240
#define SCREEN_H 135
#define FOV 100.0f
#define KEY_SHIFT ' '
#define WORLD_SIZE 512
#define MENU_ITEMS_PER_PAGE 8
#define GRAPH_HISTORY_SIZE 60
#define MAX_STRING_LEN 64

// --- Enums ---
enum BlockType {
    TYPE_AIR = 0, TYPE_GRASS, TYPE_DIRT, TYPE_STONE, TYPE_SAND, TYPE_SNOW,
    TYPE_WOOD, TYPE_LEAF, TYPE_ORE, TYPE_WATER, TYPE_ICE, TYPE_GRAVEL,
    TYPE_FLOWER, TYPE_MUSHROOM, TYPE_BRICK, TYPE_COAL, TYPE_IRON, TYPE_GOLD
};

enum MenuType { MT_FLOAT = 0, MT_BOOL = 1, MT_INT = 2, MT_SYSINFO = 3, MT_ACTION = 4, MT_REBOOT = 5, MT_CATSWITCH = 6 };

// Block Colors (565)
const uint16_t blockCols[] = {
    0x8410, 0x2E05, 0x6261, 0x738E, 0xEF55, 0xFFFF, // Grass, Dirt, Stone, Sand, Snow, Water
    0x6328, 0x16C8, 0xBC0E, 0x13AF, 0xFFFE, 0x5ACB, // Wood, Leaf, Ore, Water, Ice, Gravel
    0xF81F, 0x7B6D, 0x8800, 0x4208, 0xAD55, 0xFDE8  // Flower, Mushroom, Brick, Coal, Iron, Gold
};

// --- Global Variables ---
int tFPS = 30;
bool wire = false, showMenu = false, showHUD = true, ocMode = true;
bool flatWorld = false, vsync = false, flyMode = false, autoJump = false;
bool fixedFramerate = false;

float dayT = 0, tSpd = 0.004f, walkS = 0.22f, gravity = 0.05f, jumpP = 0.35f;
float wAmp = 1.0f, wFreq = 0.05f, fogD = 0.4f;

int seed = 42, waterLvl = 4, jumpCount = 0, biomeSize = 50, spiClock = 80;
bool autoWalk = true, isGliding = true;

float stepHeight = 1.0f, airFriction = 0.9f, swimSpeed = 0.5f;
float caveDensity = 0.3f, treeFrequency = 0.5f, oreFrequency = 0.3f;

// Player State
float pX = 128.5f, pZ = 128.5f, pY = 15.0f, pYaw = 0.0f;
float velY = 0.0f, rDist = 10.0f, sDist = 10.0f;

uint32_t gF = 0, lastInput = 0, lastFrameTime = 0, frameCount = 0, fps = 0;
uint32_t skippedFrames = 0, lastFrametime = 0;

// World Generation Parameters
float terrainAmp = 2.5f;
float terrainFreq = 0.05f;
int chunkLoadDist = 2;

// === ENGINE OPTIONS ===
int maxFaces = DEFAULT_MAX_FACES;
int maxChunks = DEFAULT_MAX_CHUNKS;
bool pointcloudMode = false;
int frameSkip = 0;

// === PHYSICS OPTIONS ===
bool enableDoubleJump = true;
bool enableGlideJump = true;
bool roamingRandom = false;

// === SYSTEM OPTIONS ===
bool showInfoScreen = false;
bool showStats = false;
bool showFrametimeGraph = false;
bool showPerfGraph = false;

// Frametime history for graphs
float frametimeHistory[GRAPH_HISTORY_SIZE] = {0};
uint32_t perfHistory[GRAPH_HISTORY_SIZE] = {0};
int historyIdx = 0;

// --- Chunk Structure ---
struct Chunk {
    uint8_t s[2048]; 
    int32_t cx, cz;
    bool a = false;
    uint32_t ls = 0;

    inline void set(int x, int y, int z, uint8_t t) {
        if (y >= 0 && y < 16 && x >= 0 && x < 16 && z >= 0 && z < 16) {
            int idx = (x << 8) | (z << 4) | y;
            int byteIdx = idx >> 1;
            if (idx & 1) { 
                s[byteIdx] = (s[byteIdx] & 0x0F) | (t << 4);
            } else { 
                s[byteIdx] = (s[byteIdx] & 0xF0) | (t & 0x0F);
            }
        }
    }

    inline uint8_t get(int x, int y, int z) {
        if (y < 0 || y >= 16 || x < 0 || x >= 16 || z < 0 || z >= 16) return 0;
        int idx = (x << 8) | (z << 4) | y;
        int byteIdx = idx >> 1;
        return (idx & 1) ? (s[byteIdx] >> 4) : (s[byteIdx] & 0x0F);
    }
};

// --- Face Structure ---
struct Face {
    uint32_t d;
    float depth;
    
    static Face pack(int x, int y, int z, int s, int t, float dp) {
        return { 
            (uint32_t)((x + 512) << 22 | (y & 15) << 18 | (z + 512) << 8 | (s & 7) << 5 | (t & 15)), 
            dp 
        };
    }
    
    int x() const { return (d >> 22) - 512; }
    int y() const { return (d >> 18) & 15; }
    int z() const { return ((d >> 8) & 0x3FF) - 512; } 
    int side() const { return (d >> 5) & 7; }
    uint16_t col() const { return blockCols[d & 0x0F]; } 
};

// --- Projection Point ---
struct Pt { int16_t x, y; bool v; };

// --- Global Arrays ---
Chunk pool[DEFAULT_MAX_CHUNKS];
Face faces[MAX_FACES]; 
int fCnt = 0;
M5Canvas cv(&M5Cardputer.Display);

// --- Menu System ---
struct MenuOpt {
    const char* name;
    void* value;
    float minVal, maxVal, step;
    uint8_t type;
};

int selCat = 0, selOpt = 0, scrollIdx = 0;
const char* cats[] = { "ENGINE", "PHYSICS", "WORLD", "SYSTEM" };

// ========== MENU DEFINITIONS ==========
MenuOpt m0[] = { 
    {"Target FPS", &tFPS, 5, 60, 5, MT_INT},
    {"Fixed Framerate", &fixedFramerate, 0, 1, 1, MT_BOOL},
    {"Render Dist", &rDist, 4, 120, 2, MT_FLOAT},
    {"LOD Dist", &sDist, 4, 120, 2, MT_FLOAT},
    {"Max Faces", &maxFaces, 0, 4000, 100, MT_INT},
    {"Max Chunks", &maxChunks, 16, 128, 8, MT_INT},
    {"Pointcloud", &pointcloudMode, 0, 1, 1, MT_BOOL},
    {"Frame Skip", &frameSkip, 0, 5, 1, MT_INT},
    {"Wireframe", &wire, 0, 1, 1, MT_BOOL},
    {"Show HUD", &showHUD, 0, 1, 1, MT_BOOL},
    {"Fog Start", &fogD, 0.1, 0.9, 0.05, MT_FLOAT}
};

MenuOpt m1[] = { 
    {"Walk Speed", &walkS, 0.05, 1.2, 0.05, MT_FLOAT},
    {"Gravity", &gravity, 0.01, 0.5, 0.01, MT_FLOAT},
    {"Jump Power", &jumpP, 0.1, 1.0, 0.05, MT_FLOAT},
    {"Double Jump", &enableDoubleJump, 0, 1, 1, MT_BOOL},
    {"Glide Jump", &enableGlideJump, 0, 1, 1, MT_BOOL},
    {"Roam Random", &roamingRandom, 0, 1, 1, MT_BOOL},
    {"Auto-Walk", &autoWalk, 0, 1, 1, MT_BOOL},
    {"Fly Mode", &flyMode, 0, 1, 1, MT_BOOL},
    {"Step Height", &stepHeight, 0.5, 2.0, 0.1, MT_FLOAT},
    {"Air Friction", &airFriction, 0.0, 1.0, 0.1, MT_FLOAT}
};

MenuOpt m2[] = { 
    {"Amplitude", &wAmp, 0.1, 8.0, 0.2, MT_FLOAT},
    {"Frequency", &wFreq, 0.005, 0.3, 0.005, MT_FLOAT},
    {"Seed Val", &seed, 0, 999, 1, MT_INT},
    {"Flat Mode", &flatWorld, 0, 1, 1, MT_BOOL},
    {"Water Lvl", &waterLvl, 0, 8, 1, MT_INT},
    {"Cave Dens", &caveDensity, 0.0, 1.0, 0.1, MT_FLOAT},
    {"Tree Freq", &treeFrequency, 0.0, 1.0, 0.1, MT_FLOAT},
    {"Ore Freq", &oreFrequency, 0.0, 1.0, 0.1, MT_FLOAT},
    {"Terrain Amp", &terrainAmp, 1.0, 10.0, 0.5, MT_FLOAT},
    {"Terrain Freq", &terrainFreq, 0.01, 0.5, 0.01, MT_FLOAT},
    {"Chunk Load Dist", &chunkLoadDist, 1, 8, 1, MT_INT},
    {"Biome Size", &biomeSize, 10, 200, 10, MT_FLOAT},
    {"Purge World", NULL, 0, 0, 0, MT_ACTION},
    {"SPI Clock", &spiClock, 10, 80, 10, MT_INT}
};

MenuOpt m3[] = { 
    {"OC Mode", &ocMode, 0, 1, 1, MT_BOOL},
    {"CPU MHz", (void*)1, 0, 0, 0, MT_SYSINFO},
    {"Heap Free", (void*)2, 0, 0, 0, MT_SYSINFO},
    {"Info Screen", &showInfoScreen, 0, 1, 1, MT_BOOL},
    {"Statistics", &showStats, 0, 1, 1, MT_BOOL},
    {"Frametime Graph", &showFrametimeGraph, 0, 1, 1, MT_BOOL},
    {"Perf Graph", &showPerfGraph, 0, 1, 1, MT_BOOL},
    {"Uptime", (void*)7, 0, 0, 0, MT_SYSINFO},
    {"Reboot", NULL, 0, 0, 0, MT_REBOOT},
    {"Category >", NULL, 0, 0, 0, MT_CATSWITCH}
};

MenuOpt* menus[] = { m0, m1, m2, m3 };

// ========== HELPER FUNCTIONS ==========

uint16_t lerpC(uint16_t c1, uint16_t c2, float t) {
    t = fmaxf(0, fminf(1, t));
    int r = ((c1 >> 11) & 31) * (1 - t) + ((c2 >> 11) & 31) * t;
    int g = ((c1 >> 5) & 63) * (1 - t) + ((c2 >> 5) & 63) * t;
    int b = (c1 & 31) * (1 - t) + (c2 & 31) * t;
    return (r << 11) | (g << 5) | b;
}

inline int worldToChunk(float w) { return (int)w >> 4; }
inline int chunkToWorld(int c) { return c << 4; }
inline int blockInChunk(float w) { return ((int)w & 15); }
inline void wrapCoord(float& c) { 
    c = fmodf(c, WORLD_SIZE); 
    if (c < 0) c += WORLD_SIZE; 
}

inline bool canProcessInput(uint32_t& lastTime, uint32_t minInterval) {
    uint32_t now = millis();
    if (now - lastTime >= minInterval) { lastTime = now; return true; }
    return false;
}

void formatMenuValue(const MenuOpt& opt, char* buf, size_t len) {
    if (!opt.value) {
        if (opt.type == MT_SYSINFO) {
            int id = (int)(uintptr_t)opt.name; 
            if (id == 1) snprintf(buf, len, "%d MHz", ESP.getCpuFreqMHz());
            else if (id == 2) snprintf(buf, len, "%d KB", ESP.getFreeHeap() / 1024);
            else if (id == 7) snprintf(buf, len, "%lds", millis() / 1000);
            else snprintf(buf, len, "INFO");
        } else {
            snprintf(buf, len, "---");
        }
        return;
    }
    switch (opt.type) {
        case MT_FLOAT: snprintf(buf, len, "%.3f", *(float*)opt.value); break;
        case MT_BOOL:  snprintf(buf, len, "%s", *(bool*)opt.value ? "ON" : "OFF"); break;
        case MT_INT:   snprintf(buf, len, "%d", *(int*)opt.value); break;
        default:       snprintf(buf, len, "---"); break;
    }
}

bool adjustMenuValue(MenuOpt& opt, bool increase) {
    if (!opt.value) return false;
    switch (opt.type) {
        case MT_BOOL:
            *(bool*)opt.value = !*(bool*)opt.value;
            return true;
        case MT_INT: {
            int& v = *(int*)opt.value;
            v = constrain(v + (increase ? (int)opt.step : -(int)opt.step), (int)opt.minVal, (int)opt.maxVal);
            return true;
        }
        case MT_FLOAT: {
            float& v = *(float*)opt.value;
            v = constrain(v + (increase ? opt.step : -opt.step), opt.minVal, opt.maxVal);
            return true;
        }
        default: return false;
    }
}

void drawVerticalGraph(int x, int y, int w, int h, const float* data, int count, float maxVal, uint16_t color, const char* label) {
    cv.drawRect(x, y, w, h, 0x07FF);
    cv.setTextColor(0x8410);
    cv.setCursor(x + 2, y - 8);
    cv.print(label);
    int innerW = w - 4, innerH = h - 4;
    for (int i = 0; i < count; i++) {
        int px = x + 2 + (i * innerW / count);
        int barH = (int)(constrain(data[i], 0, maxVal) / maxVal * innerH);
        cv.drawFastVLine(px, y + h - 3 - barH, barH, color);
    }
}

inline int getMenuCount(int cat) { 
    if (cat == 0) return 11; 
    if (cat == 1) return 10; 
    if (cat == 2) return 14; 
    if (cat == 3) return 10; 
    return 0;
}

Pt proj(float x, float y, float z, float s, float c) {
    float tx = x - pX, ty = y - (pY + 1.6f), tz = z - pZ;
    float rx = tx * c - tz * s, rz = tx * s + tz * c;
    if (rz <= 0.15f) return {0, 0, false};
    return { (int16_t)(120 + rx * FOV / rz), (int16_t)(67 - ty * FOV / rz), true };
}

// ========== SKY & LIGHTING ==========
namespace Sky {
    struct ShootingStar {
        float x, y, dx, dy; int life, maxLife; bool active; uint16_t col;
    };
    ShootingStar sStars[3];

    uint32_t starHash(uint32_t x) {
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        return (x >> 16) ^ x;
    }

    uint16_t getSkyColor(float sunY) {
        float t[5] = {
            fmaxf(0.0f, fminf(1.0f, (sunY + 0.90f) * 1.2f)),
            fmaxf(0.0f, fminf(1.0f, (sunY + 0.65f) * 2.0f)),
            fmaxf(0.0f, fminf(1.0f, (sunY + 0.35f) * 2.5f)),
            fmaxf(0.0f, fminf(1.0f, (sunY + 0.12f) * 4.0f)),
            fmaxf(0.0f, fminf(1.0f, (sunY - 0.05f) * 4.5f))
        };
        uint16_t cols[] = {0x0000, 0x000A, 0x0812, 0x2128, 0xFBE0, 0x841F};
        uint16_t result = cols[0];
        for (int i = 0; i < 5; i++) result = lerpC(result, cols[i+1], t[i]);
        return result;
    }

    void drawSky(float sunY) {
        uint16_t top = getSkyColor(sunY), bottom = lerpC(top, 0x9492, fmaxf(0.0f, sunY + 0.25f) * 0.48f);
        for (int y = 0; y < SCREEN_H; y++) {
            float t = (float)y / SCREEN_H;
            cv.drawFastHLine(0, y, SCREEN_W, lerpC(top, bottom, t));
        }
        if (sunY >= 0.35f) return;
        
        float starAlpha = powf(fmaxf(0.0f, fminf(1.0f, (0.35f - sunY) * 2.8f)), 2.2f);
        uint16_t starBase = (sunY > -0.3f) ? 0xFFE0 : 0xFFFF;
        
        for (int i = 0; i < 160; i++) {
            uint32_t h = starHash(i + 42);
            int sx = ((int)(h % 4800) - 2400 - (int)(pYaw * 200)) % SCREEN_W;
            if (sx < 0) sx += SCREEN_W;
            int sy = (h / 4800) % (SCREEN_H - 10);
            float tw = (sinf(gF * 0.09f + i * 0.7f) * 0.5f) + 0.5f;
            if (tw > 0.12f) {
                cv.drawPixel(sx, sy, lerpC(top, starBase, tw * starAlpha));
                if (i % 25 == 0 && starAlpha > 0.8f) cv.drawPixel(sx + 1, sy, lerpC(top, 0xFFFF, starAlpha * 0.4f));
            }
        }
        for (int i = 0; i < 3; i++) {
            auto& s = sStars[i];
            if (!s.active && (starHash(gF + i) % 1500 == 0) && sunY < -0.25f) {
                s = { (float)(rand()%SCREEN_W), (float)(rand()%45), 
                      (float)((rand()%12+6)*(rand()%2?1:-1)), (float)(rand()%6+3),
                      10+rand()%18, 10+rand()%18, true, i==0?0xFFFF:(i==1?0x87FF:0xFD20) };
            }
            if (s.active) {
                float r = (float)s.life / s.maxLife;
                cv.drawLine((int)s.x, (int)s.y, (int)(s.x - s.dx*r*2.5f), (int)(s.y - s.dy*r*2.5f), lerpC(top, s.col, r));
                s.x += s.dx; s.y += s.dy;
                if (--s.life <= 0) s.active = false;
            }
        }
    }

    void drawCelestial(int x, int y, bool isDay, float sunY) {
        float maxR = isDay ? 55.0f : 32.0f;
        uint16_t sky = getSkyColor(sunY), body = isDay ? 0xFFE1 : 0xFFFF;
        float intensity = fmaxf(0.0f, fminf(1.0f, fabsf(sunY) * 3.8f));
        for (float r = maxR; r > 0; r -= 0.7f) {
            float bloom = powf(1.0f - (r/maxR), 2.8f) * intensity;
            cv.drawCircle(x, y, (int)r, lerpC(sky, body, bloom));
            if (r < 2.5f) cv.fillCircle(x, y, 3, body);
        }
        if (!isDay) {
            uint16_t cr = 0x7BEF;
            cv.fillCircle(x-5,y-3,2,cr); cv.fillCircle(x+3,y+5,1,cr); cv.fillCircle(x+6,y-1,3,cr);
            cv.drawCircle(x, y, 6, 0xAD75);
        } else {
            for (int i=0;i<6;i++) cv.drawPixel(x+(starHash(gF+i)%15)-7, y+(starHash(gF+i+10)%15)-7, 0xFFFF);
        }
    }
}

namespace Light {
    inline float getIntensity(float sunY) { return fmaxf(0.1f, sunY * 0.5f + 0.5f); }
}

// ========== CHUNK & BLOCK LOGIC ==========

Chunk* getChunk(int32_t cx, int32_t cz) {
    for (int i = 0; i < maxChunks; i++) {
        if (pool[i].a && pool[i].cx == cx && pool[i].cz == cz) {
            pool[i].ls = gF;
            return &pool[i];
        }
    }
    return nullptr;
}

void generateChunk(int32_t cx, int32_t cz, bool isCritical) {
    if (getChunk(cx, cz)) return;
    int idx = -1; uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < maxChunks; i++) {
        if (!pool[i].a) { idx = i; break; }
        if (pool[i].ls < oldest) { oldest = pool[i].ls; idx = i; }
    }
    if (idx == -1) return;
    
    Chunk& ch = pool[idx];
    ch = { {}, cx, cz, true, isCritical ? (gF + 10) : gF };
    
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            int h = (int)(6.0f + sinf((cx*16+x)*terrainFreq)*terrainAmp + cosf((cz*16+z)*terrainFreq)*terrainAmp);
            for (int y = 0; y < 16; y++) {
                uint8_t t = (y <= h) ? (y > h-1 ? TYPE_GRASS : TYPE_STONE) : (y < waterLvl ? TYPE_WATER : TYPE_AIR);
                ch.set(x, y, z, t);
            }
        }
    }
}

void updateChunkRing() {
    int32_t tx = worldToChunk(pX), tz = worldToChunk(pZ);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            generateChunk(tx+dx, tz+dz, true);
    for (int dz = -chunkLoadDist; dz <= chunkLoadDist; dz++)
        for (int dx = -chunkLoadDist; dx <= chunkLoadDist; dx++)
            generateChunk(tx+dx, tz+dz, false);
}

inline uint8_t getBlock(int32_t x, int y, int32_t z) {
    if (y < 0 || y >= 16) return 0;
    Chunk* c = getChunk(x >> 4, z >> 4);
    return c ? c->get(x & 15, y, z & 15) : 0;
}

// ========== MENU ==========

void drawMenu() {
    cv.fillRect(5, 5, 230, 125, cv.color565(10, 10, 25));
    cv.drawRect(5, 5, 230, 125, 0x07FF);
    cv.setTextColor(0xFFE0);
    cv.setCursor(10, 10);
    cv.printf("[%s]", cats[selCat]);
    cv.drawLine(5, 22, 235, 22, 0x07FF);

    MenuOpt* opts = menus[selCat];
    int count = getMenuCount(selCat);
    char buf[MAX_STRING_LEN];

    for (int i = 0; i < MENU_ITEMS_PER_PAGE && scrollIdx + i < count; i++) {
        int idx = scrollIdx + i;
        cv.setTextColor(idx == selOpt ? 0xFFFF : 0x8410);
        cv.setCursor(12, 30 + i * 11);
        cv.printf("%s%s", idx == selOpt ? "> " : "", opts[idx].name);
        cv.setCursor(140, 30 + i * 11);
        formatMenuValue(opts[idx], buf, MAX_STRING_LEN);
        cv.print(buf);
    }
}

// ========== PERFORMANCE & GRAPHS ==========

void drawStats() {
    if (!showStats) return;
    cv.setTextColor(0x8410);
    cv.setCursor(5, SCREEN_H - 95);
    cv.printf("Mem:%dKB CPU:%dMHz", ESP.getFreeHeap()/1024, ESP.getCpuFreqMHz());
    cv.setCursor(5, SCREEN_H - 85);
    cv.printf("Up:%ds FPS:%d", millis()/1000, fps);
}

void drawInfoScreen() {
    if (!showInfoScreen) return;
    cv.fillRect(30, 20, 180, 95, cv.color565(15, 15, 35));
    cv.drawRect(30, 20, 180, 95, 0x07FF);
    cv.setTextColor(0xFFE0);
    cv.setCursor(35, 25); cv.print("=== VOXEL ENGINE ===");
    cv.setCursor(35, 40); cv.printf("Pos: %.1f,%.1f,%.1f", pX, pY, pZ);
    cv.setCursor(35, 52); cv.printf("Faces: %d/%d", fCnt, maxFaces);
    cv.setCursor(35, 64); cv.printf("Chunks: %d/%d", maxChunks, (int)MAX_CHUNKS);
    cv.setCursor(35, 76); cv.printf("Day: %.2f Light: %.2f", dayT, Light::getIntensity(sinf(dayT)));
    cv.setCursor(35, 88); cv.printf("Mode: %s", pointcloudMode ? "POINTCLOUD" : "MESH");
}

void updatePerformance() {
    uint32_t now = millis();
    frametimeHistory[historyIdx] = (float)(now - lastFrametime);
    perfHistory[historyIdx] = fCnt;
    historyIdx = (historyIdx + 1) % GRAPH_HISTORY_SIZE;
    lastFrametime = now;
    
    static uint32_t lastAdj = 0;
    if (now - lastAdj > 1000) {
        lastAdj = now;
        fps = frameCount;
        frameCount = 0;
        if (fixedFramerate) {
            if (fps < tFPS && rDist > 4) rDist -= 1.0f;
            else if (fps > tFPS && rDist < 120) rDist += 1.0f;
        }
    }
    frameCount++;
}

// ========== RENDERING ==========

void collectFaces(float s, float c) {
    fCnt = 0;
    sDist += (rDist - sDist) * 0.1f;
    float maxDist2 = sDist * sDist, fogStart2 = maxDist2 * fogD, lodThresh2 = 54.0f * 54.0f;
    int pcx = worldToChunk(pX), pcz = worldToChunk(pZ), radius = ceil(sDist / 16);
    
    static const int8_t faceNorm[] = { 0,1,0, 0,-1,0, 0,0,1, 0,0,-1, 1,0,0, -1,0,0 };
    
    for (int dz = -radius; dz <= radius && fCnt < maxFaces; dz++) {
        for (int dx = -radius; dx <= radius && fCnt < maxFaces; dx++) {
            Chunk* ch = getChunk(pcx+dx, pcz+dz);
            if (!ch) continue;
            
            int wx0 = chunkToWorld(pcx+dx), wz0 = chunkToWorld(pcz+dz);
            float cdx = (wx0+8) - pX, cdz = (wz0+8) - pZ;
            bool useLOD = (cdx*cdx + cdz*cdz) > lodThresh2;
            
            if (useLOD) {
                for (int lx = 0; lx < 16 && fCnt < maxFaces; lx += 2) {
                    for (int lz = 0; lz < 16 && fCnt < maxFaces; lz += 2) {
                        int h = -1; uint8_t t = 0;
                        for (int ly = 15; ly >= 0; ly--) if ((t = ch->get(lx,ly,lz))) { h = ly; break; }
                        if (h < 0) continue;
                        float rdx = (wx0+lx) - pX, rdz = (wz0+lz) - pZ;
                        if (rdx*s + rdz*c < -2.0f) continue;
                        faces[fCnt++] = Face::pack(wx0+lx, h, wz0+lz, 0, t, rdx*rdx + rdz*rdz);
                    }
                }
            } else {
                for (int i = 0; i < 4096 && fCnt < maxFaces; i++) {
                    int lx = (i>>8)&15, ly = i&15, lz = (i>>4)&15;
                    uint8_t t = ch->get(lx, ly, lz);
                    if (!t) continue;
                    int wx = wx0+lx, wz = wz0+lz;
                    float rdx = wx+0.5f-pX, rdz = wz+0.5f-pZ, d2 = rdx*rdx+rdz*rdz;
                    if (d2 > maxDist2 || rdx*s+rdz*c < -1.5f) continue;
                    for (int j = 0; j < 6 && fCnt < maxFaces; j++) {
                        if (faceNorm[j*3]*rdx + faceNorm[j*3+1]*(ly-pY-1.6f) + faceNorm[j*3+2]*rdz >= 0) continue;
                        if (!getBlock(wx+faceNorm[j*3], ly+faceNorm[j*3+1], wz+faceNorm[j*3+2]))
                            faces[fCnt++] = Face::pack(wx, ly, wz, j, t, d2);
                    }
                }
            }
        }
    }
}

void drawFace(const Face& f, float s, float c, uint16_t skyTop, uint16_t fogColor, float maxDist2, float fogStart2, bool wireframe, bool pointcloud) {
    uint16_t col = f.col();
    if (f.side() != 0) col = (col >> 1) & 0x7BEF;
    col = lerpC(0x0000, col, Light::getIntensity(sinf(dayT)));
    
    if (f.depth > fogStart2) {
        float fogF = fminf(1.0f, (f.depth - fogStart2) / (maxDist2 - fogStart2));
        col = lerpC(col, fogColor, fogF);
    }
    
    static const int8_t faceVerts[] = {
        0,1,0, 1,1,0, 1,1,1, 0,1,1, 0,0,0, 1,0,0, 1,0,1, 0,0,1,
        0,0,1, 1,0,1, 1,1,1, 0,1,1, 0,0,0, 1,0,0, 1,1,0, 0,1,0,
        1,0,0, 1,0,1, 1,1,1, 1,1,0, 0,0,0, 0,0,1, 0,1,1, 0,1,0
    };
    
    if (pointcloud || f.depth > 54.0f*54.0f) {
        Pt p = proj(f.x()+0.5f, f.y()+1.0f, f.z()+0.5f, s, c);
        if (p.v) {
            if (pointcloud) cv.drawPixel(p.x, p.y, col);
            else cv.fillRect(p.x, p.y, 2, 2, wireframe ? 0xAAAA : col);
        }
        return;
    }
    
    Pt v[4]; bool visible = true;
    const int8_t* verts = &faceVerts[f.side() * 12];
    for (int k = 0; k < 4; k++) {
        v[k] = proj(f.x()+verts[k*3], f.y()+verts[k*3+1], f.z()+verts[k*3+2], s, c);
        if (!v[k].v) { visible = false; break; }
    }
    if (!visible) return;
    
    if (wireframe) {
        for (int k = 0; k < 4; k++) cv.drawLine(v[k].x, v[k].y, v[(k+1)%4].x, v[(k+1)%4].y, 0xFFFF);
    } else {
        cv.fillTriangle(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, col);
        cv.fillTriangle(v[0].x, v[0].y, v[2].x, v[2].y, v[3].x, v[3].y, col);
    }
}

void render() {
    if (frameSkip > 0 && gF % (frameSkip + 1) != 0) { cv.pushSprite(0, 0); return; }
    
    gF++; dayT += tSpd;
    float s = sinf(pYaw), c = cosf(pYaw), sunY = sinf(dayT);
    uint16_t skyTop = Sky::getSkyColor(sunY), fogColor = lerpC(skyTop, 0x9492, fmaxf(0.0f, sunY+0.25f)*0.45f);
    
    updateChunkRing();
    Sky::drawSky(sunY);
    
    Pt astro = proj(pX+cosf(dayT)*120, pY+sunY*120, pZ+sinf(dayT)*120, s, c);
    if (astro.v) {
        Sky::drawCelestial(astro.x, astro.y, sunY > -0.1f, sunY);
    }
    
    collectFaces(s, c);
    std::sort(faces, faces + fCnt, [](const Face& a, const Face& b) { return a.depth > b.depth; });
    
    float maxDist2 = sDist * sDist, fogStart2 = maxDist2 * fogD;
    for (int i = 0; i < fCnt; i++) {
        drawFace(faces[i], s, c, skyTop, fogColor, maxDist2, fogStart2, wire, pointcloudMode);
    }
    
    if (showHUD) { cv.setCursor(5,5); cv.setTextColor(0xFFFF); cv.printf("FPS:%d | Faces:%d/%d", fps, fCnt, maxFaces); }
    drawStats();
    if (showFrametimeGraph) drawVerticalGraph(5, SCREEN_H-40, SCREEN_W-10, 35, frametimeHistory, GRAPH_HISTORY_SIZE, 33.3f, 0x07E0, "Frametime(ms)");
    if (showPerfGraph) drawVerticalGraph(5, SCREEN_H-80, SCREEN_W-10, 35, (float*)perfHistory, GRAPH_HISTORY_SIZE, (float)maxFaces, 0x001F, "Faces");
    drawInfoScreen();
    
    updatePerformance();
}

// ========== SETUP & LOOP ==========

void setup() {
    M5.begin(); 
    M5Cardputer.begin();
    cv.createSprite(SCREEN_W, SCREEN_H);
    setCpuFrequencyMhz(240);
    randomSeed(analogRead(0));
    updateChunkRing();
}

void handleMenuInput() {
    MenuOpt* opts = menus[selCat];
    int count = getMenuCount(selCat);
    
    if (M5Cardputer.Keyboard.isKeyPressed(';') && canProcessInput(lastInput, 100))
        selOpt = (selOpt > 0) ? selOpt - 1 : count - 1;
    if (M5Cardputer.Keyboard.isKeyPressed('.') && canProcessInput(lastInput, 100))
        selOpt = (selOpt < count - 1) ? selOpt + 1 : 0;
    
    if (M5Cardputer.Keyboard.isKeyPressed('/') && canProcessInput(lastInput, 150)) {
        MenuOpt& o = opts[selOpt];
        if (adjustMenuValue(o, true)) return;
        if (o.type == MT_ACTION) { 
            for(int i=0;i<maxChunks;i++) pool[i].a=0; 
            updateChunkRing(); 
        }
        else if (o.type == MT_REBOOT) ESP.restart();
        else if (o.type == MT_CATSWITCH) { selCat = (selCat+1)%4; selOpt=scrollIdx=0; }
    }
    if (M5Cardputer.Keyboard.isKeyPressed(',') && canProcessInput(lastInput, 150))
        adjustMenuValue(opts[selOpt], false);
    
    if (selOpt < scrollIdx) scrollIdx = selOpt;
    else if (selOpt >= scrollIdx + MENU_ITEMS_PER_PAGE) scrollIdx = selOpt - MENU_ITEMS_PER_PAGE + 1;
}

void handleMovement(float& dx, float& dz, float s, float c) {
    if (roamingRandom && !showMenu) {
        static uint32_t lastRoam = 0; static float roamAng = 0;
        if (millis() - lastRoam > 2000) { roamAng = (rand() % 628) / 100.0f; lastRoam = millis(); }
        dx = sinf(pYaw + roamAng) * walkS * 0.5f;
        dz = cosf(pYaw + roamAng) * walkS * 0.5f;
        return;
    }
    
    auto& k = M5Cardputer.Keyboard; // FIX: Removed ()
    if (k.isKeyPressed('q')) pYaw -= 0.1;
    if (k.isKeyPressed('e')) pYaw += 0.1;
    if ((k.isKeyPressed('w') || autoWalk) && !roamingRandom) { dx = s * walkS; dz = c * walkS; }
    if (k.isKeyPressed('s')) { dx = -s * walkS; dz = -c * walkS; }
}

bool handleCollision(float dx, float dz) {
    if (!getBlock(pX+dx, pY+0.5, pZ+dz)) { pX += dx; pZ += dz; return true; }
    if (!getBlock(pX+dx, pY+1.5, pZ+dz)) { pX += dx; pZ += dz; pY += 1.0f; return true; }
    return false;
}

void handleJumping() {
    if (!M5Cardputer.Keyboard.isKeyPressed(' ') || !canProcessInput(lastInput, 200)) return;
    
    if (jumpCount == 0) { velY = jumpP; jumpCount = 1; }
    else if (jumpCount == 1 && enableDoubleJump) { velY = jumpP * 0.8f; jumpCount = 2; }
    else if (enableGlideJump) { isGliding = true; }
}

void loop() {
    M5Cardputer.update();
    auto& k = M5Cardputer.Keyboard; // FIX: Removed ()
    
    if (k.isKeyPressed('0') && canProcessInput(lastInput, 200)) {
        if (showMenu) selCat = (selCat + 1) % 4;
        showMenu = true;
    }
    if (k.isKeyPressed(KEY_BACKSPACE)) showMenu = false;
    
    if (showMenu) handleMenuInput();
    
    float dx = 0, dz = 0, s = sinf(pYaw), c = cosf(pYaw);
    handleMovement(dx, dz, s, c);
    handleCollision(dx, dz);
    
    wrapCoord(pX); wrapCoord(pZ);
    
    velY -= (enableGlideJump && isGliding && velY < 0) ? gravity * 0.2f : gravity;
    pY += velY;
    if (getBlock(pX, pY, pZ)) { pY = ceilf(pY); velY = 0; jumpCount = 0; isGliding = false; }
    
    if (autoJump && !getBlock(pX, pY-1, pZ) && jumpCount == 0) { velY = jumpP; jumpCount = 1; }
    handleJumping();
    
    if (flyMode) {
        if (k.isKeyPressed(' ')) velY = 0.2f;
        if (k.isKeyPressed(KEY_SHIFT)) velY = -0.2f;
    }
    
    render();
    if (showMenu) drawMenu();
    cv.pushSprite(0, 0);
    
    if (fixedFramerate) {
        uint32_t ft = millis() - lastFrameTime, target = 1000 / tFPS;
        if (ft < target) delay(target - ft);
        lastFrameTime = millis();
    }
}
