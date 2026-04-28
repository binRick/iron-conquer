#include "pathfind.h"

#include <limits.h>
#include <stdlib.h>

#define NODE_COUNT (MAP_WIDTH * MAP_HEIGHT)
#define IDX(x, y) ((y) * MAP_WIDTH + (x))

bool tile_passable(const TileMap *map, int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) return false;
    if (map->blocked[y][x]) return false;
    TileType t = map->tiles[y][x];
    return t == TILE_GRASS || t == TILE_DIRT || t == TILE_ORE;
}

typedef struct { int node; int f; } PQItem;
typedef struct { PQItem items[NODE_COUNT]; int len; } PQ;

static void pq_push(PQ *q, int node, int f) {
    int i = q->len++;
    q->items[i].node = node;
    q->items[i].f    = f;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (q->items[p].f <= q->items[i].f) break;
        PQItem tmp = q->items[p];
        q->items[p] = q->items[i];
        q->items[i] = tmp;
        i = p;
    }
}

static PQItem pq_pop(PQ *q) {
    PQItem top = q->items[0];
    q->items[0] = q->items[--q->len];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, m = i;
        if (l < q->len && q->items[l].f < q->items[m].f) m = l;
        if (r < q->len && q->items[r].f < q->items[m].f) m = r;
        if (m == i) break;
        PQItem tmp = q->items[m];
        q->items[m] = q->items[i];
        q->items[i] = tmp;
        i = m;
    }
    return top;
}

static int heuristic_octile(int x1, int y1, int x2, int y2) {
    int dx = abs(x1 - x2), dy = abs(y1 - y2);
    int dmin = dx < dy ? dx : dy;
    return 10 * (dx + dy) - 6 * dmin;
}

bool pathfind(const TileMap *map, int sx, int sy, int gx, int gy, Path *out) {
    out->len = 0;
    if (!tile_passable(map, sx, sy)) return false;
    if (!tile_passable(map, gx, gy)) return false;
    if (sx == gx && sy == gy) return true;

    static int  g_score[NODE_COUNT];
    static int  came_from[NODE_COUNT];
    static bool closed[NODE_COUNT];
    for (int i = 0; i < NODE_COUNT; ++i) {
        g_score[i]   = INT_MAX;
        came_from[i] = -1;
        closed[i]    = false;
    }

    static PQ pq;
    pq.len = 0;

    int start_idx = IDX(sx, sy);
    g_score[start_idx] = 0;
    pq_push(&pq, start_idx, heuristic_octile(sx, sy, gx, gy));

    static const int dx8[8]   = {  1, -1,  0,  0,  1,  1, -1, -1 };
    static const int dy8[8]   = {  0,  0,  1, -1,  1, -1,  1, -1 };
    static const int cost8[8] = { 10, 10, 10, 10, 14, 14, 14, 14 };

    while (pq.len > 0) {
        PQItem cur = pq_pop(&pq);
        if (closed[cur.node]) continue;
        closed[cur.node] = true;

        int cx = cur.node % MAP_WIDTH;
        int cy = cur.node / MAP_WIDTH;

        if (cx == gx && cy == gy) {
            static int rev[NODE_COUNT];
            int rev_len = 0;
            int idx = IDX(gx, gy);
            while (idx != start_idx) {
                rev[rev_len++] = idx;
                idx = came_from[idx];
                if (idx < 0) break;
            }
            int n = rev_len;
            if (n > MAX_PATH) n = MAX_PATH;
            for (int i = 0; i < n; ++i) {
                int p = rev[rev_len - 1 - i];
                out->xs[i] = p % MAP_WIDTH;
                out->ys[i] = p / MAP_WIDTH;
            }
            out->len = n;
            return true;
        }

        for (int d = 0; d < 8; ++d) {
            int nx = cx + dx8[d];
            int ny = cy + dy8[d];
            if (!tile_passable(map, nx, ny)) continue;
            if (d >= 4) {
                if (!tile_passable(map, cx + dx8[d], cy)) continue;
                if (!tile_passable(map, cx, cy + dy8[d])) continue;
            }
            int nidx = IDX(nx, ny);
            if (closed[nidx]) continue;
            int tentative = g_score[cur.node] + cost8[d];
            if (tentative < g_score[nidx]) {
                g_score[nidx]   = tentative;
                came_from[nidx] = cur.node;
                int f = tentative + heuristic_octile(nx, ny, gx, gy);
                pq_push(&pq, nidx, f);
            }
        }
    }
    return false;
}
