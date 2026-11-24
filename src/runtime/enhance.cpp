#include "runtime/enhance.h"
#include "runtime/enhance_diagnostics.h"
#include "runtime/rom_import.h"
#include "runtime/scenery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace rt {

namespace {
// The widened car list (hook_car_view_cone) must not reach the race logic:
// the game stores how many cars it draws (LOD != 0), shifted by a course
// value, at 0x5014f7 (0x8404), and its car AI reads that at 0x11418 and
// 0x15d70. Cars listed only thanks to the wider cone are counted here and
// taken back out before that store, so the race sees the original count.
bool wide_cars_on() {
    static const bool on = [] {
        const char *v = std::getenv("M2_WIDE_CAR_CULL");
        return !(v && v[0] == '0');
    }();
    return on;
}
bool current_car_extra = false; // the car being listed passed only the wider cone
uint32_t extra_drawn = 0;       // such cars that got a level of detail this list
} // namespace

void hook_scenery_object(Cpu &c) {
    static std::FILE *trace = [] {
        const char *p = std::getenv("M2_SCENERY_OBJECT_TRACE");
        return p && *p ? std::fopen(p, "w") : nullptr;
    }();
    if (!trace) return;
    float f12, f13;
    std::memcpy(&f12, &c.m_r[12], 4);
    std::memcpy(&f13, &c.m_r[13], 4);
    std::fprintf(trace, "%08x,%08x,%08x,%u,%g,%g\n", c.m_r[11], c.m_r[12], c.m_r[13], c.m_r[14], f12, f13);
}

void hook_car_list_begin(Cpu &) {
    extra_drawn = 0;
    current_car_extra = false;
}

void hook_car_view_cone(Cpu &c) {
    current_car_extra = false;
    const auto *state = c.bus->scenery();
    if (!wide_cars_on() || !state || state->width <= 496) return;
    // g0 = int(|x|) - 5; compare as if |x| were seen in the original width.
    const int32_t g0 = int32_t(c.m_r[16]), z = int32_t(c.m_r[10]);
    const int64_t scaled = ((int64_t(g0) + 5) * 496) / state->width - 5;
    c.m_r[16] = uint32_t(int32_t(scaled));
    current_car_extra = z <= g0 && z > int32_t(scaled); // dropped by the original test only
}

static bool near_model() {
    static const bool on = [] {
        const char *v = std::getenv("M2_CAR_NEAR_MODEL");
        return v && v[0] == '1';
    }();
    return on;
}

// M2_CAR_NEAR_MAX=K (experiment): only the K nearest listed cars get the near
// model; the others keep their own. 0 = no limit.
static int near_max() {
    static const int k = [] {
        const char *v = std::getenv("M2_CAR_NEAR_MAX");
        return v ? std::atoi(v) : 0;
    }();
    return k;
}

void hook_car_list_lod(Cpu &c) {
    // 0x96b8: stob r9,0xd0(r6); r9 = the car's level of detail (0: not drawn).
    static std::FILE *lod_trace = [] {
        const char *path = std::getenv("M2_CAR_LOD_TRACE");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (lod_trace) std::fprintf(lod_trace, "%u,%u,%d\n", c.m_r[6], c.m_r[9], int32_t(c.m_r[10]));
    // Levels measured on Beginner (r10 = int(z)): 4 up to z 20 (near body
    // 0x8808), 3 to 35 (0x8c00), 2 to 100 (0x9000), 1 beyond (0x93dc).
    if (near_model() && c.m_r[9] != 0) c.m_r[9] = 4;
    if (current_car_extra && c.m_r[9] != 0) ++extra_drawn;
    current_car_extra = false;
}

void hook_car_lod_counts(Cpu &c) {
    if (!near_model()) return;
    if (near_max() > 0) {
        // every listed car is stored at level 4 and takes the models in
        // distance order (0x82dc), falling to the next level once a model's
        // quota runs out: the K nearest get the near model, the others the mid
        const uint32_t all = c.m_r[12] + c.m_r[13] + c.m_r[14] + c.m_r[15];
        c.m_r[15] = std::min(all, uint32_t(near_max()));
        c.m_r[14] = all - c.m_r[15];
        c.m_r[12] = c.m_r[13] = 0;
        return;
    }
    c.m_r[15] += c.m_r[12] + c.m_r[13] + c.m_r[14]; // 0x96fc: r12..r15 = cars per level 1..4
    c.m_r[12] = c.m_r[13] = c.m_r[14] = 0;
}

void hook_car_lod_budget(Cpu &c) {
    static const bool lifted = [] {
        const char *v = std::getenv("M2_CAR_LOD_BUDGET");
        return v && v[0] == '1';
    }();
    // r3 = 0x7d0, the allowance about to be subtracted
    if (lifted || near_model()) c.m_r[3] = 1u << 20;
}

void hook_car_count_store(Cpu &c) {
    // 0x8400: shro r3,r8,r8; r8 = cars drawn this frame, before the course shift.
    c.m_r[8] = c.m_r[8] >= extra_drawn ? c.m_r[8] - extra_drawn : 0;
}

void hook_mobile_slot_reset(Cpu &c) {
    c.bus->reset_mobile_geometry_slot(c.m_r[15]);
}

// The draw list holds at most 63 cells (0x5016c1..0x5016ff; the game uses
// 0x501700), so the widest square is 7x7 (49).
// The game's polygon budget per frame: objects stop being drawn once their
// costs pass it; the original allowance is set once at boot. Costs come
// from object-list metadata before clipping,
// not visible polygons; a later list is rejected after prior lists exceed it.
constexpr uint32_t kBudgetAddr = 0x5010f4, kGameBudget = 5000;

void hook_draw_list(Cpu &c) {
    const int level = std::clamp(Enhance::draw_distance, Enhance::kDrawMin, Enhance::kDrawMax);
    auto *state = c.bus->scenery();
    static bool legacy_changed = false; // non-board callers of the old distance hook
    bool &changed = state ? state->budget_changed : legacy_changed;
    const uint32_t budget = state ? state->budget(level) : automatic_scenery_budget(496, level);
    const bool wide = state && state->width > 496 && !state->original_selection;
    if (budget != kGameBudget || (state && state->custom_budget) || level > 0 || changed) {
        c.bus->write_dword(kBudgetAddr, budget);
        changed = budget != kGameBudget;
    }
    const uint32_t car = c.m_r[8];
    if (state && state->diagnostics) {
        state->measured.car_cell.reset();
        if (car <= 255) state->measured.car_cell = uint8_t(car);
        ++state->measured.list_updates;
        state->measured.original_cells = c.bus->read_byte(0x5016c0);
        state->measured.selected_cells = state->measured.original_cells;
        state->measured.exclusion_mask = c.m_r[13];
        state->measured.direction_mask = c.m_r[9];
    }
    if (state && state->omit_cell) {
        const unsigned count = std::min<unsigned>(c.bus->read_byte(0x5016c0), 63);
        unsigned kept = 0;
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t cell = c.bus->read_byte(0x5016c1 + i);
            if (cell == *state->omit_cell) continue;
            if (kept != i) c.bus->write_byte(0x5016c1 + kept, cell);
            ++kept;
        }
        if (kept != count) c.bus->write_byte(0x5016c0, uint8_t(kept));
        ++state->measured.omit_hooks;
        state->measured.omitted_cells += count - kept;
        if (state->diagnostics) state->measured.selected_cells = kept;
    }
    if (level == 0 && !wide) return;
    if (car > 255) return; // not a cell: leave the game's list alone
    const int cx = int(car & 15), cy = int(car >> 4);
    uint8_t list[63];
    int n = 0;
    if (level <= 0) {
        // Less: the game's own list, cut to the car's cell (-2) or one cell around it (-1).
        const int keep = level == -2 ? 0 : level == -1 ? 1 : 2;
        const int count = std::min<int>(c.bus->read_byte(0x5016c0), 63);
        for (int i = 0; i < count; i++) {
            const int cell = c.bus->read_byte(0x5016c1 + uint32_t(i));
            const int dx = (cell & 15) - cx, dy = (cell >> 4) - cy;
            if (std::max(std::abs(dx), std::abs(dy)) <= keep) list[n++] = uint8_t(cell);
        }
        if (wide) {
            // r13 excludes invalid grid cells and course-specific hidden areas;
            // r9 is the original directional mask. Keep the former and supply
            // all remaining candidates in range to the widened geometrizer.
            // This conservative superset works for every camera direction.
            // Preserve the original prefix/order; append missing cells by ring.
            for (int radius = 0; radius <= keep; ++radius)
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                        const int x = cx + dx, y = cy + dy;
                        if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                        const unsigned slot = unsigned((dy + 2) * 5 + dx + 2);
                        if (c.m_r[13] & (1u << slot)) continue;
                        const auto cell = uint8_t(x + 16 * y);
                        if (std::find(list, list + n, cell) == list + n && n < 63) list[n++] = cell;
                    }
        }
    } else {
        // More: every cell within 2 (+1: the whole 5x5 the game chooses from)
        // or 3 (+2: 7x7) of the car's, nearest ring first, inside the
        // 16x16 grid (cells are bytes; no wrapping into the next row).
        // Local play build, +2 "horizon" (M2_DRAW_HORIZON=0 off): the 63-cell
        // list cannot hold a 9x9 square, so after the 5x5 around the car the
        // free entries go to cells up to 6 away in the direction the game is
        // looking, taken from its own directional mask r9 (5x5 slots, slot =
        // (dy + 2) * 5 + dx + 2), nearest first; behind-the-camera rings are
        // not spent. No direction in the mask: the plain 7x7.
        static const bool horizon_on = [] {
            const char *v = std::getenv("M2_DRAW_HORIZON");
            return !(v && v[0] == '0');
        }();
        int hx = 0, hy = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (c.m_r[9] & (1u << unsigned((dy + 2) * 5 + dx + 2))) hx += dx, hy += dy;
        const bool horizon = horizon_on && level == 2 && (hx || hy);
        const int radius = horizon ? 2 : 1 + level;
        for (int r = 0; r <= radius; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                    list[n++] = uint8_t(x + 16 * y);
                }
        if (horizon) {
            // Stable horizon (a tester saw a forest at the side of a wide view blink
            // in and out): the mask's direction is coarse and the 63-entry cap
            // reorders cells, so (1) the heading is smoothed across updates,
            // (2) the cone follows the real view: the original cone is
            // |x| < z (45 degrees), widened by width / 496, plus a margin for
            // the cells' size, and (3) a cell listed last update keeps its
            // place, first, while within a wider cone still.
            static double sx = 0, sy = 0;
            static bool listed[256] = {};
            const double hl = std::hypot(double(hx), double(hy));
            sx = 0.7 * sx + 0.3 * (hx / hl), sy = 0.7 * sy + 0.3 * (hy / hl);
            const double sl = std::hypot(sx, sy);
            const double ux = sl > 0 ? sx / sl : hx / hl, uy = sl > 0 ? sy / sl : hy / hl;
            const int width = state ? state->width : 496;
            const double half_view = std::atan(double(std::max(width, 496)) / 496.0);
            const double add_cos = std::cos(std::min(half_view + 0.35, 3.0)),  // about +20 degrees
                         keep_cos = std::cos(std::min(half_view + 0.61, 3.0)); // about +35 degrees
            // The native skyline tower is in cell 0x91 on Daytona Revision A.
            // Feed that cell through the game's usual geometry and materials, so
            // the terrain, depth ordering and viewport still determine visibility.
            constexpr uint32_t kCourseAddr = 0x501460;
            constexpr uint8_t kExpertCourse = 1, kSkylineTowerCell = 0x91, kBridgeArchCell = 0x89;
            const bool expert = std::strcmp(M2_ROMSET, "daytona") == 0
                && c.bus->read_byte(kCourseAddr) == kExpertCourse;
            static const bool extend_tower = [] {
                const char *v = std::getenv("M2_EXPERT_TOWER");
                return v && std::strcmp(v, "1") == 0;
            }();
            const bool tower_approach = cx > 1 && cx <= 10 && std::abs(cy - 9) <= 1;
            struct Far { int rank; double d; double cosine; uint8_t cell; };
            Far far[256];
            int m = 0;
            for (int dy = -6; dy <= 6; ++dy)
                for (int dx = -6; dx <= 6; ++dx) {
                    const int ring = std::max(std::abs(dx), std::abs(dy));
                    if (ring <= 2) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                    const double d = std::hypot(double(dx), double(dy));
                    const double cosine = (dx * ux + dy * uy) / d;
                    const auto cell = uint8_t(x + 16 * y);
                    if (listed[cell] && cosine >= keep_cos) far[m++] = {0, d, cosine, cell};
                    else if (cosine >= add_cos) far[m++] = {1, d, cosine, cell};
                }
            if (extend_tower && expert && tower_approach && cx > 7) {
                const int dx = 1 - cx, dy = 9 - cy;
                const double d = std::hypot(double(dx), double(dy));
                const double cosine = (dx * ux + dy * uy) / d;
                if (cosine >= add_cos) far[m++] = {1, d, cosine, kSkylineTowerCell};
            }
            // A total order: many cells share a distance, and an unstable tie
            // would swap which of them fit under the cap from one update to
            // the next (a blinking cell). Nearest, then most central, then cell.
            std::sort(far, far + m, [](const Far &a, const Far &b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                if (a.d != b.d) return a.d < b.d;
                if (a.cosine != b.cosine) return a.cosine > b.cosine;
                return a.cell < b.cell;
            });
            const auto landmark = [expert, tower_approach, cy](const Far &f) {
                if (!expert) return false;
                // Approach from the straight, not the other side of the skyline.
                if (f.cell == kSkylineTowerCell) return tower_approach;
                // Let the canyon turn reveal the arch before prioritizing it.
                constexpr double kBridgeApproachCos = 0.7071067811865476; // 45 degrees
                return f.cell == kBridgeArchCell && cy >= 12 && f.cosine >= kBridgeApproachCos;
            };
            int regular_slots = 63 - n - int(std::count_if(far, far + m, landmark));
            std::fill(std::begin(listed), std::end(listed), false);
            for (const Far &f : std::span(far, m)) {
                if (!landmark(f)) {
                    if (regular_slots == 0) continue;
                    --regular_slots;
                }
                list[n++] = f.cell;
                listed[f.cell] = true;
            }
            // Up to 63 cells cost more than the automatic +2 allowance (Expert
            // peak 15879 against 15000: 258 frames dropped objects). Unless
            // the player set a custom allowance, give the horizon room.
            if (!(state && state->custom_budget)) {
                constexpr uint32_t kHorizonBudget = 30000;
                if (budget < kHorizonBudget) c.bus->write_dword(kBudgetAddr, kHorizonBudget);
                changed = true;
            }
        }
    }
    if (EnhanceDiagnostics::draw_order_only && level > 0) {
        // Rank the original entries by the generated rings. Keep every
        // occurrence and leave cells outside the rings in original order.
        uint8_t rank[256];
        std::fill_n(rank, 256, uint8_t(n));
        for (int i = 0; i < n; ++i) rank[list[i]] = uint8_t(i);
        n = std::min<int>(c.bus->read_byte(0x5016c0), 63);
        for (int i = 0; i < n; ++i)
            list[i] = c.bus->read_byte(0x5016c1 + uint32_t(i));
        std::stable_sort(list, list + n, [&](uint8_t a, uint8_t b) { return rank[a] < rank[b]; });
    }
    c.bus->write_byte(0x5016c0, uint8_t(n));
    for (int i = 0; i < n; i++) c.bus->write_byte(0x5016c1 + uint32_t(i), list[i]);
    if (state && state->diagnostics) state->measured.selected_cells = uint32_t(n);
}

void hook_scenery_cost(Cpu &c) {
    auto *state = c.bus->scenery();
    if (state && state->diagnostics)
        state->measured.cost_peak = std::max(state->measured.cost_peak, c.m_r[4]);
}

void hook_scenery_budget_check(Cpu &c) {
    auto *state = c.bus->scenery();
    if (!state || !state->diagnostics) return;
    ++state->measured.budget_checks;
    state->measured.cost_peak = std::max(state->measured.cost_peak, c.m_r[5]);
    if (c.m_r[5] > c.m_r[4]) ++state->measured.budget_rejections;
}

} // namespace rt
