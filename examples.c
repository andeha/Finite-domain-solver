/*
 * examples.c -- five small CSPs solved by the same library.
 *
 *   1. label placement   -- forbidden_pairs from box-overlap geometry
 *   2. scheduling        -- alldiff + precedence + machine pinning
 *   3. configuration     -- allowed_pairs compatibility tables
 *   4. layout            -- 2x2 floorplan with adjacency requirements
 *   5. timetabling       -- (room, slot) packing with teacher conflicts
 *
 * Each example posts its constraints, asks the solver for the first
 * solution, and prints it. They share the same csp_t API and propagator
 * set; only the modelling differs.
 */
#include "csp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- *
 * 1. Label placement
 *
 * Each city gets a label with four candidate corners (NW/NE/SW/SE).
 * For every pair of cities we precompute which corner combinations
 * cause the labels' bounding boxes to overlap, and post that as a
 * forbidden_pairs constraint.
 * ---------------------------------------------------------------- */

#define LBL_W 10
#define LBL_H 4

typedef struct { int x, y; const char *name; } city_t;

static const city_t CITIES[] = {
    { 5,  10, "Stockholm"},
    {35,  15, "Goteborg"},
    {65,  12, "Malmo"},
    {20,  30, "Uppsala"},
    {50,  32, "Linkoping"},
};
#define N_CITIES (sizeof CITIES / sizeof CITIES[0])

static const char *CORNER[] = {"NW", "NE", "SW", "SE"};

static void label_box(int cx, int cy, int corner, int *bx, int *by) {
    switch (corner) {
    case 0: *bx = cx - LBL_W; *by = cy - LBL_H; break;   /* NW */
    case 1: *bx = cx + 1;     *by = cy - LBL_H; break;   /* NE */
    case 2: *bx = cx - LBL_W; *by = cy + 1;     break;   /* SW */
    case 3: *bx = cx + 1;     *by = cy + 1;     break;   /* SE */
    }
}

static int boxes_overlap(int ax, int ay, int bx, int by) {
    return ax < bx + LBL_W && bx < ax + LBL_W &&
           ay < by + LBL_H && by < ay + LBL_H;
}

static int print_labels(const csp_t *csp, void *ud) {
    const var_t *L = ud;
    for (size_t i = 0; i < N_CITIES; i++)
        printf("  %-12s label sits %s of the marker\n",
               CITIES[i].name, CORNER[csp_value(csp, L[i])]);
    return 0;                                /* first solution is enough */
}

static void example_label_placement(void) {
    printf("=== 1. Label placement ===\n");
    csp_t *csp = csp_new();

    var_t L[N_CITIES];
    for (size_t i = 0; i < N_CITIES; i++) L[i] = csp_var(csp, 0, 3);

    for (size_t i = 0; i < N_CITIES; i++) {
        for (size_t j = i + 1; j < N_CITIES; j++) {
            int32_t bad[16][2];
            uint32_t n_bad = 0;
            for (int p = 0; p < 4; p++) {
                int ax, ay; label_box(CITIES[i].x, CITIES[i].y, p, &ax, &ay);
                for (int q = 0; q < 4; q++) {
                    int bx, by; label_box(CITIES[j].x, CITIES[j].y, q, &bx, &by);
                    if (boxes_overlap(ax, ay, bx, by)) {
                        bad[n_bad][0] = p;
                        bad[n_bad][1] = q;
                        n_bad++;
                    }
                }
            }
            if (n_bad)
                csp_forbidden_pairs(csp, L[i], L[j],
                                    (const int32_t (*)[2])bad, n_bad);
        }
    }

    if (csp_solve(csp, print_labels, L) == 0)
        printf("  no feasible placement\n");
    csp_free(csp);
    printf("\n");
}

/* ---------------------------------------------------------------- *
 * 2. Scheduling
 *
 * Four jobs assigned to cells in a 2-machine x 3-timeslot grid.
 * Encoding: cell = slot*N_MACHINES + machine, so cell/2 is slot and
 * cell%2 is machine. alldiff forbids two jobs in the same cell;
 * unary constraints pin specific machines; forbidden_pairs encode
 * "job A finishes before job B".
 * ---------------------------------------------------------------- */

#define MACHINES  2
#define SLOTS     3
#define CELLS     (MACHINES * SLOTS)

typedef struct { const char *name; int forced_machine; } job_t;

static const job_t JOBS[] = {
    {"Mill A",   -1},   /* any machine                                  */
    {"Lathe B",   1},   /* machine 1 only (only lathe in shop)          */
    {"Drill C",  -1},
    {"Pack D",   -1},
};
#define N_JOBS (sizeof JOBS / sizeof JOBS[0])

/* (a, b): all (cell_a, cell_b) where slot(a) >= slot(b). */
static void post_precedence(csp_t *csp, var_t a, var_t b) {
    int32_t bad[CELLS * CELLS][2];
    uint32_t n = 0;
    for (int ca = 0; ca < CELLS; ca++)
        for (int cb = 0; cb < CELLS; cb++)
            if ((ca / MACHINES) >= (cb / MACHINES)) {
                bad[n][0] = ca; bad[n][1] = cb; n++;
            }
    csp_forbidden_pairs(csp, a, b, (const int32_t (*)[2])bad, n);
}

static int print_schedule(const csp_t *csp, void *ud) {
    const var_t *J = ud;
    for (int s = 0; s < SLOTS; s++) {
        printf("  slot %d:", s);
        for (int m = 0; m < MACHINES; m++) {
            int cell = s * MACHINES + m;
            const char *who = "      .  ";
            for (size_t i = 0; i < N_JOBS; i++)
                if (csp_value(csp, J[i]) == cell) who = JOBS[i].name;
            printf("  M%d=%-10s", m, who);
        }
        printf("\n");
    }
    return 0;
}

static void example_scheduling(void) {
    printf("=== 2. Scheduling ===\n");
    csp_t *csp = csp_new();

    var_t J[N_JOBS];
    for (size_t i = 0; i < N_JOBS; i++) J[i] = csp_var(csp, 0, CELLS - 1);

    /* Pin jobs that need a specific machine. */
    for (size_t i = 0; i < N_JOBS; i++) {
        if (JOBS[i].forced_machine < 0) continue;
        for (int s = 0; s < SLOTS; s++)
            for (int m = 0; m < MACHINES; m++)
                if (m != JOBS[i].forced_machine)
                    csp_neq_c(csp, J[i], s * MACHINES + m);
    }

    /* No two jobs share a cell. */
    csp_alldiff(csp, J, N_JOBS);

    /* Precedence: Mill A (0) before Drill C (2); Drill C before Pack D (3). */
    post_precedence(csp, J[0], J[2]);
    post_precedence(csp, J[2], J[3]);

    if (csp_solve(csp, print_schedule, J) == 0)
        printf("  schedule infeasible\n");
    csp_free(csp);
    printf("\n");
}

/* ---------------------------------------------------------------- *
 * 3. Configuration
 *
 * Four-component PC build with allowed_pairs compatibility tables.
 * The solver intersects the tables transitively through propagation:
 * a CPU/board pair narrows the board, which narrows the RAM, etc.
 * ---------------------------------------------------------------- */

enum { CPU_AMD9, CPU_AMD7, CPU_INT13, N_CPU };
enum { MB_X670,  MB_B650,  MB_Z790,   N_MB  };
enum { RAM_DDR5_6000, RAM_DDR5_5200, RAM_DDR4_3200, N_RAM };
enum { GPU_NV40, GPU_AMD9070, N_GPU };

static const char *CPU_NAME[] = {"Ryzen 9",       "Ryzen 7",       "Core i5"};
static const char *MB_NAME[]  = {"X670 (AM5)",    "B650 (AM5)",    "Z790 (LGA1700)"};
static const char *RAM_NAME[] = {"DDR5-6000",     "DDR5-5200",     "DDR4-3200"};
static const char *GPU_NAME[] = {"RTX 4090",      "RX 9070"};

static const int32_t CPU_MB[][2] = {
    {CPU_AMD9, MB_X670},  {CPU_AMD9, MB_B650},        /* AM5 sockets    */
    {CPU_AMD7, MB_X670},  {CPU_AMD7, MB_B650},
    {CPU_INT13, MB_Z790},                             /* Intel platform */
};
static const int32_t MB_RAM[][2] = {
    {MB_X670, RAM_DDR5_6000}, {MB_X670, RAM_DDR5_5200},
    {MB_B650, RAM_DDR5_5200},                         /* B650 only 5200 */
    {MB_Z790, RAM_DDR5_6000}, {MB_Z790, RAM_DDR4_3200},
};
static const int32_t CPU_GPU[][2] = {                 /* PSU headroom   */
    {CPU_AMD9, GPU_NV40},
    {CPU_AMD7, GPU_NV40}, {CPU_AMD7, GPU_AMD9070},
    {CPU_INT13, GPU_AMD9070},
};

typedef struct { var_t cpu, mb, ram, gpu; } build_t;

static int print_build(const csp_t *csp, void *ud) {
    const build_t *b = ud;
    printf("  CPU: %s\n", CPU_NAME[csp_value(csp, b->cpu)]);
    printf("  MB : %s\n", MB_NAME [csp_value(csp, b->mb)]);
    printf("  RAM: %s\n", RAM_NAME[csp_value(csp, b->ram)]);
    printf("  GPU: %s\n", GPU_NAME[csp_value(csp, b->gpu)]);
    return 0;
}

static void example_configuration(void) {
    printf("=== 3. Configuration ===\n");
    csp_t *csp = csp_new();
    build_t b = {
        csp_var(csp, 0, N_CPU - 1),
        csp_var(csp, 0, N_MB  - 1),
        csp_var(csp, 0, N_RAM - 1),
        csp_var(csp, 0, N_GPU - 1),
    };
    csp_allowed_pairs(csp, b.cpu, b.mb,
                      CPU_MB,  sizeof CPU_MB  / sizeof CPU_MB[0]);
    csp_allowed_pairs(csp, b.mb,  b.ram,
                      MB_RAM,  sizeof MB_RAM  / sizeof MB_RAM[0]);
    csp_allowed_pairs(csp, b.cpu, b.gpu,
                      CPU_GPU, sizeof CPU_GPU / sizeof CPU_GPU[0]);

    /* Customer wants the AMD9070. Watch the rest of the build collapse. */
    csp_eq_c(csp, b.gpu, GPU_AMD9070);

    if (csp_solve(csp, print_build, &b) == 0)
        printf("  no compatible build\n");
    csp_free(csp);
    printf("\n");
}

/* ---------------------------------------------------------------- *
 * 4. Layout
 *
 * Four rooms in a 2x2 grid: 0=NW, 1=NE, 2=SW, 3=SE.
 * Adjacency graph (share a wall): 0-1, 0-2, 1-3, 2-3.
 * Diagonals (do not share a wall): 0-3, 1-2.
 * Constraints:
 *   - all four rooms in distinct cells
 *   - kitchen and living room must share a wall
 *   - bedroom and bathroom must share a wall
 *   - kitchen and bathroom must NOT share a wall (smell, hygiene)
 * ---------------------------------------------------------------- */

static const int32_t ADJACENT[][2] = {
    {0,1}, {1,0}, {0,2}, {2,0}, {1,3}, {3,1}, {2,3}, {3,2},
};
static const int32_t NOT_ADJACENT[][2] = {
    {0,3}, {3,0}, {1,2}, {2,1},                         /* the diagonals */
};

typedef struct { var_t living, kitchen, bedroom, bath; } house_t;

static int print_house(const csp_t *csp, void *ud) {
    const house_t *h = ud;
    static const char *cells[] = {"NW", "NE", "SW", "SE"};
    int v[4] = {-1, -1, -1, -1};
    v[csp_value(csp, h->living )] = 0;
    v[csp_value(csp, h->kitchen)] = 1;
    v[csp_value(csp, h->bedroom)] = 2;
    v[csp_value(csp, h->bath   )] = 3;
    static const char *room[] = {"Living", "Kitchen", "Bedroom", "Bath"};
    for (int i = 0; i < 4; i++)
        printf("  %s -> %s\n", cells[i], v[i] >= 0 ? room[v[i]] : "(empty)");
    return 0;
}

static void example_layout(void) {
    printf("=== 4. Layout ===\n");
    csp_t *csp = csp_new();
    house_t h = {
        csp_var(csp, 0, 3),
        csp_var(csp, 0, 3),
        csp_var(csp, 0, 3),
        csp_var(csp, 0, 3),
    };
    var_t all[] = { h.living, h.kitchen, h.bedroom, h.bath };
    csp_alldiff(csp, all, 4);

    csp_allowed_pairs (csp, h.living,  h.kitchen,
                       ADJACENT,     sizeof ADJACENT     / sizeof ADJACENT[0]);
    csp_allowed_pairs (csp, h.bedroom, h.bath,
                       ADJACENT,     sizeof ADJACENT     / sizeof ADJACENT[0]);
    csp_allowed_pairs (csp, h.kitchen, h.bath,
                       NOT_ADJACENT, sizeof NOT_ADJACENT / sizeof NOT_ADJACENT[0]);

    if (csp_solve(csp, print_house, &h) == 0)
        printf("  no feasible floorplan\n");
    csp_free(csp);
    printf("\n");
}

/* ---------------------------------------------------------------- *
 * 5. Timetabling
 *
 * Four courses, two rooms, three slots: 6 cells in total. Cell c
 * encodes slot = c/N_ROOMS, room = c%N_ROOMS.
 *   - alldiff: no two courses in the same cell
 *   - shared-teacher pairs: must land in different slots
 *   - one course is bound to the lab (room 1)
 * ---------------------------------------------------------------- */

#define N_ROOMS 2
#define N_SLOTS 3
#define N_CELLS (N_ROOMS * N_SLOTS)

static const char *COURSE[] = {"Algorithms", "Compilers", "Networks", "OS Lab"};
#define N_COURSES (sizeof COURSE / sizeof COURSE[0])

/* Two cells share a slot iff cell/N_ROOMS is equal. */
static void post_different_slot(csp_t *csp, var_t a, var_t b) {
    int32_t bad[N_CELLS * N_CELLS][2];
    uint32_t n = 0;
    for (int x = 0; x < N_CELLS; x++)
        for (int y = 0; y < N_CELLS; y++)
            if (x / N_ROOMS == y / N_ROOMS) {
                bad[n][0] = x; bad[n][1] = y; n++;
            }
    csp_forbidden_pairs(csp, a, b, (const int32_t (*)[2])bad, n);
}

static int print_timetable(const csp_t *csp, void *ud) {
    const var_t *C = ud;
    for (int s = 0; s < N_SLOTS; s++) {
        printf("  slot %d:", s);
        for (int r = 0; r < N_ROOMS; r++) {
            int cell = s * N_ROOMS + r;
            const char *who = "       .   ";
            for (size_t i = 0; i < N_COURSES; i++)
                if (csp_value(csp, C[i]) == cell) who = COURSE[i];
            printf("  R%d=%-12s", r, who);
        }
        printf("\n");
    }
    return 0;
}

static void example_timetabling(void) {
    printf("=== 5. Timetabling ===\n");
    csp_t *csp = csp_new();

    var_t C[N_COURSES];
    for (size_t i = 0; i < N_COURSES; i++)
        C[i] = csp_var(csp, 0, N_CELLS - 1);

    csp_alldiff(csp, C, N_COURSES);

    /* Algorithms (0) and Compilers (1) share Prof. Olsson. */
    post_different_slot(csp, C[0], C[1]);
    /* Networks (2) and OS Lab (3) share Dr. Lindberg. */
    post_different_slot(csp, C[2], C[3]);

    /* OS Lab must run in the lab room (room 1) -- ban room 0 cells. */
    for (int s = 0; s < N_SLOTS; s++)
        csp_neq_c(csp, C[3], s * N_ROOMS + 0);

    if (csp_solve(csp, print_timetable, C) == 0)
        printf("  no feasible timetable\n");
    csp_free(csp);
    printf("\n");
}

/* ---------------------------------------------------------------- */

int main(void) {
    example_label_placement();
    example_scheduling();
    example_configuration();
    example_layout();
    example_timetabling();
    return 0;
}
