#include "item_icons.h"
#include "game_archive.h"
#include "icon_decode.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ervr {
namespace {

constexpr uint32_t kIconSide = 256;   // a belt item held up to the eyes; the wheel shrinks it to 128. The game's own are 1024

struct Img { uint32_t w = 0, h = 0; std::vector<uint32_t> px; };
std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<int> g_queue;
std::unordered_set<int> g_known;          // queued, loaded or missing: asked for at most once
std::unordered_map<int, Img> g_img;
std::atomic<long> g_loaded{0}, g_failed{0};
bool g_started = false;

double NowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// One icon: archive -> TPF -> the MENU_Knowledge_<id> DDS -> RGBA -> 256 px.
bool Load(int id, Img& im, std::string& why) {
    std::vector<uint8_t> tpf;
    if (!ArchiveReadItemIcon(id, tpf, &why)) return false;
    std::vector<TpfTexture> tex;
    if (!TpfTextures(tpf.data(), tpf.size(), tex)) { why = "not a TPF"; return false; }
    for (const TpfTexture& t : tex) {
        if (MenuIconId(t.name) != id || !t.dds) continue;
        std::vector<uint32_t> full; uint32_t w = 0, h = 0;
        if (!DdsDecode(t.dds, t.ddsLen, full, w, h)) {
            DdsInfo d; DdsParse(t.dds, t.ddsLen, d);
            why = std::string("a ") + DdsKindName(d.kind) + " texture this build cannot decode";
            return false;
        }
        ShrinkRgba(full, w, h, kIconSide, im.px, im.w, im.h);
        return true;
    }
    why = "no MENU_Knowledge texture inside";
    return false;
}

void Loader() {
    for (;;) {
        int id = -1;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            id = g_queue.front();
            g_queue.pop_front();
        }
        const double t0 = NowMs();
        Img im; std::string why;
        if (!Load(id, im, why)) {
            const long n = g_failed.fetch_add(1) + 1;
            if (n <= 12) LOGI("[icons] icon %d unavailable: %s - its slot shows the slot number", id, why.c_str());
            continue;
        }
        const long n = g_loaded.fetch_add(1) + 1;
        if (n <= 6) LOGI("[icons] icon %d read from the archive (%ux%u, %.0f ms)", id, im.w, im.h, NowMs() - t0);
        std::lock_guard<std::mutex> g(g_mtx);
        g_img[id] = std::move(im);
    }
}

} // namespace

void ItemIconsRequest(int iconId) {
    if (iconId < 0) return;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        if (!g_known.insert(iconId).second) return;
        g_queue.push_back(iconId);
        if (!g_started) { g_started = true; std::thread(Loader).detach(); }
    }
    g_cv.notify_one();
}

bool ItemIconRgba(int iconId, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h) {
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_img.find(iconId);
    if (it == g_img.end()) return false;
    rgba = it->second.px; w = it->second.w; h = it->second.h;
    return true;
}

ItemIconStats ItemIconsGetStats() {
    ItemIconStats s;
    s.loaded = g_loaded.load(); s.failed = g_failed.load();
    return s;
}

}
