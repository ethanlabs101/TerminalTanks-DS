// Headless melonDS test harness for Terminal Tanks DS.
//
//   harness run  <rom.nds> <script> <outdir> [sd.img]       one console (optional SD card image: saves persist)
//   harness mp   <rom.nds> <scriptA> <scriptB> <outdir>     two consoles linked by melonDS LocalMP (local wireless)
//
// Scripts (one command per line, frame numbers are absolute):
//   <frame> press <KEY>        KEY = A B SELECT START RIGHT LEFT UP DOWN R L X Y
//   <frame> release <KEY>
//   <frame> tap <KEY>          press for 4 frames
//   <frame> touch <x> <y>      / <frame> untouch
//   <frame> shot <name>        writes <outdir>/<name>.ppm (both screens stacked, 256x384)
//   <frame> end
// Both consoles run in their own thread (LocalMP blocks while waiting for replies, like real hardware).
// The ARM9 debug output (nocash "print" via the emulated debug port) is captured into <outdir>/log_<i>.txt.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "Platform.h"
#include "SPI.h"
#include "net/LocalMP.h"

using namespace melonDS;

static LocalMP *g_mp = nullptr;
static thread_local FILE *t_log = nullptr;   /* the running instance's debug log (nocash prints arrive via Log(Debug)) */
static std::string g_outdir;

struct Inst {
    int id;
    NDS *nds;
    FILE *log;
};

namespace melonDS::Platform {

static bool FILE_EXISTS(const std::string &n) { FILE *f = fopen(n.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
void SignalStop(StopReason, void *) {}
std::string GetLocalFilePath(const std::string &f) { return f; }
FileHandle *OpenFile(const std::string &path, FileMode mode) {
    bool rd = mode & FileMode::Read, wr = mode & FileMode::Write;
    bool preserve = mode & FileMode::Preserve, nocreate = mode & FileMode::NoCreate, append = mode & FileMode::Append;
    if (wr && (preserve || append)) {
        FILE *t = fopen(path.c_str(), append ? "ab" : "r+b");
        if (t || nocreate) return (FileHandle *)t;
        return (FileHandle *)fopen(path.c_str(), rd ? "w+b" : "wb");
    }
    if (wr) return nocreate && !FILE_EXISTS(path) ? nullptr : (FileHandle *)fopen(path.c_str(), rd ? "w+b" : "wb");
    return (FileHandle *)fopen(path.c_str(), "rb");
}
FileHandle *OpenLocalFile(const std::string &path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string &n) { FILE *f = fopen(n.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
bool LocalFileExists(const std::string &n) { return FileExists(n); }
bool CheckFileWritable(const std::string &) { return true; }
bool CheckLocalFileWritable(const std::string &) { return true; }
bool CloseFile(FileHandle *f) { return fclose((FILE *)f) == 0; }
bool IsEndOfFile(FileHandle *f) { return feof((FILE *)f); }
bool FileReadLine(char *s, int n, FileHandle *f) { return fgets(s, n, (FILE *)f) != nullptr; }
u64 FilePosition(FileHandle *f) { return (u64)ftell((FILE *)f); }
bool FileSeek(FileHandle *f, s64 off, FileSeekOrigin o) {
    int w = o == FileSeekOrigin::Start ? SEEK_SET : o == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseek((FILE *)f, (long)off, w) == 0;
}
void FileRewind(FileHandle *f) { rewind((FILE *)f); }
u64 FileRead(void *d, u64 s, u64 c, FileHandle *f) { return fread(d, s, c, (FILE *)f); }
bool FileFlush(FileHandle *f) { return fflush((FILE *)f) == 0; }
u64 FileWrite(const void *d, u64 s, u64 c, FileHandle *f) { return fwrite(d, s, c, (FILE *)f); }
u64 FileWriteFormatted(FileHandle *f, const char *fmt, ...) {
    va_list a; va_start(a, fmt); int n = vfprintf((FILE *)f, fmt, a); va_end(a); return n;
}
u64 FileLength(FileHandle *f) { long p = ftell((FILE *)f); fseek((FILE *)f, 0, SEEK_END); long l = ftell((FILE *)f); fseek((FILE *)f, p, SEEK_SET); return l; }
void Log(LogLevel level, const char *fmt, ...) {
    if (level == LogLevel::Debug && t_log) { va_list a; va_start(a, fmt); vfprintf(t_log, fmt, a); va_end(a); fflush(t_log); return; }
    if (level < LogLevel::Warn && !getenv("HARNESS_VERBOSE")) return;
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}
struct Thread { std::thread t; };
Thread *Thread_Create(std::function<void()> f) { auto *t = new Thread; t->t = std::thread(f); return t; }
void Thread_Free(Thread *t) { if (t->t.joinable()) t->t.detach(); delete t; }
void Thread_Wait(Thread *t) { if (t->t.joinable()) t->t.join(); }
struct Semaphore { std::mutex m; std::condition_variable cv; int count = 0; };
Semaphore *Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore *s) { delete s; }
void Semaphore_Reset(Semaphore *s) { std::lock_guard<std::mutex> l(s->m); s->count = 0; }
void Semaphore_Wait(Semaphore *s) { std::unique_lock<std::mutex> l(s->m); s->cv.wait(l, [&] { return s->count > 0; }); s->count--; }
bool Semaphore_TryWait(Semaphore *s, int ms) {
    std::unique_lock<std::mutex> l(s->m);
    if (!ms) { if (s->count > 0) { s->count--; return true; } return false; }
    if (!s->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return s->count > 0; })) return false;
    s->count--;
    return true;
}
void Semaphore_Post(Semaphore *s, int c) { { std::lock_guard<std::mutex> l(s->m); s->count += c; } s->cv.notify_all(); }
struct Mutex { std::mutex m; };
Mutex *Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex *m) { delete m; }
void Mutex_Lock(Mutex *m) { m->m.lock(); }
void Mutex_Unlock(Mutex *m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex *m) { return m->m.try_lock(); }
void Sleep(u64 us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
u64 GetMSCount() { return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 GetUSCount() { return (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void WriteNDSSave(const u8 *, u32, u32, u32, void *) {}
void WriteGBASave(const u8 *, u32, u32, u32, void *) {}
void WriteFirmware(const Firmware &, u32, u32, void *) {}
void WriteDateTime(int, int, int, int, int, int, void *) {}
static int inst_of(void *u) { return u ? ((Inst *)u)->id : 0; }
void MP_Begin(void *u) { g_mp->Begin(inst_of(u)); }
void MP_End(void *u) { g_mp->End(inst_of(u)); }
int MP_SendPacket(u8 *d, int l, u64 ts, void *u) { return g_mp->SendPacket(inst_of(u), d, l, ts); }
int MP_RecvPacket(u8 *d, u64 *ts, void *u) { return g_mp->RecvPacket(inst_of(u), d, ts); }
int MP_SendCmd(u8 *d, int l, u64 ts, void *u) { return g_mp->SendCmd(inst_of(u), d, l, ts); }
int MP_SendReply(u8 *d, int l, u64 ts, u16 aid, void *u) { return g_mp->SendReply(inst_of(u), d, l, ts, aid); }
int MP_SendAck(u8 *d, int l, u64 ts, void *u) { return g_mp->SendAck(inst_of(u), d, l, ts); }
int MP_RecvHostPacket(u8 *d, u64 *ts, void *u) { return g_mp->RecvHostPacket(inst_of(u), d, ts); }
u16 MP_RecvReplies(u8 *d, u64 ts, u16 m, void *u) { return g_mp->RecvReplies(inst_of(u), d, ts, m); }
int Net_SendPacket(u8 *, int, void *) { return 0; }
int Net_RecvPacket(u8 *, void *) { return 0; }
void Camera_Start(int, void *) {}
void Camera_Stop(int, void *) {}
void Camera_CaptureFrame(int, u32 *, int, int, bool, void *) {}
void Mic_Start(void *) {}
void Mic_Stop(void *) {}
int Mic_ReadInput(s16 *, int, void *) { return 0; }
AACDecoder *AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder *) {}
bool AAC_Configure(AACDecoder *, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder *, const void *, int, void *, int) { return false; }
bool Addon_KeyDown(KeyType, void *) { return false; }
void Addon_RumbleStart(u32, void *) {}
void Addon_RumbleStop(void *) {}
float Addon_MotionQuery(MotionQueryType, void *) { return 0; }
DynamicLibrary *DynamicLibrary_Load(const char *) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary *) {}
void *DynamicLibrary_LoadFunction(DynamicLibrary *, const char *) { return nullptr; }

} // namespace melonDS::Platform

struct Cmd { int frame; std::string op; std::vector<std::string> a; };

static std::vector<Cmd> load_script(const std::string &path) {
    std::vector<Cmd> v;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Cmd c;
        ss >> c.frame >> c.op;
        std::string x;
        while (ss >> x) c.a.push_back(x);
        v.push_back(c);
    }
    return v;
}

static int keybit(const std::string &k) {
    static const std::map<std::string, int> m = {{"A", 0}, {"B", 1}, {"SELECT", 2}, {"START", 3}, {"RIGHT", 4}, {"LEFT", 5}, {"UP", 6},
                                                 {"DOWN", 7}, {"R", 8}, {"L", 9}, {"X", 10}, {"Y", 11}};
    auto it = m.find(k);
    return it == m.end() ? -1 : it->second;
}

static void shot(NDS *nds, const std::string &path) {
    void *top, *bot;
    nds->GPU.GetFramebuffers(&top, &bot);
    FILE *f = fopen(path.c_str(), "wb");
    fprintf(f, "P6\n256 384\n255\n");
    for (int s = 0; s < 2; s++) {
        u32 *fb = (u32 *)(s ? bot : top);
        for (int i = 0; i < 256 * 192; i++) {
            u32 c = fb[i];
            unsigned char px[3] = {(unsigned char)((c >> 16) & 255), (unsigned char)((c >> 8) & 255), (unsigned char)(c & 255)};
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

static std::vector<u8> read_file(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::atomic<int> g_done{0};

static void run_instance(int id, const std::string &rom, const std::string &script, const std::string &sdimg) {
    Inst *inst = new Inst{id, nullptr, nullptr};
    std::string logp = g_outdir + "/log_" + std::to_string(id) + ".txt";
    inst->log = fopen(logp.c_str(), "w");
    t_log = inst->log;
    NDSArgs args;
    args.JIT = std::nullopt;
    // distinct MAC per console so dswifi sees two different machines
    args.Firmware = Firmware(0);
    auto mac = args.Firmware.GetHeader().MacAddr;
    mac[5] = (u8)(0x10 + id);
    args.Firmware.GetHeader().MacAddr = mac;
    args.Firmware.UpdateChecksums();
    NDS *nds = new NDS(std::move(args), inst);
    inst->nds = nds;
    nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    auto data = read_file(rom);
    std::unique_ptr<u8[]> romdata(new u8[data.size()]);
    memcpy(romdata.get(), data.data(), data.size());
    NDSCart::NDSCartArgs cargs{};
    if (!sdimg.empty()) {
        cargs.SDCard = FATStorageArgs{sdimg, 64ull * 1024 * 1024, false, std::nullopt};
    }
    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)data.size(), inst, std::move(cargs));
    if (!cart) { fprintf(stderr, "bad rom\n"); g_done++; return; }
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot(rom);
    nds->Start();
    auto cmds = load_script(script);
    u32 keys = 0xFFF;
    int last = 0;
    for (auto &c : cmds) last = std::max(last, c.frame);
    std::map<int, int> release_at;
    size_t ci = 0;
    for (int f = 0; f <= last; f++) {
        for (auto it = release_at.begin(); it != release_at.end();) {
            if (it->second == f) { keys |= 1u << it->first; it = release_at.erase(it); }
            else ++it;
        }
        while (ci < cmds.size() && cmds[ci].frame == f) {
            Cmd &c = cmds[ci++];
            if (c.op == "press") keys &= ~(1u << keybit(c.a[0]));
            else if (c.op == "release") keys |= 1u << keybit(c.a[0]);
            else if (c.op == "tap") { keys &= ~(1u << keybit(c.a[0])); release_at[keybit(c.a[0])] = f + 4; }
            else if (c.op == "touch") nds->TouchScreen((u16)std::stoi(c.a[0]), (u16)std::stoi(c.a[1]));
            else if (c.op == "untouch") nds->ReleaseScreen();
            else if (c.op == "shot") shot(nds, g_outdir + "/" + c.a[0] + ".ppm");
        }
        nds->SetKeyMask(keys);
        nds->RunFrame();
    }
    fclose(inst->log);
    g_done++;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: harness run rom script outdir [sd.img] | harness mp rom scriptA scriptB outdir\n");
        return 1;
    }
    std::string mode = argv[1];
    g_mp = new LocalMP();
    if (mode == "run") {
        g_outdir = argv[4];
        run_instance(0, argv[2], argv[3], argc > 5 ? argv[5] : "");
    } else {
        g_outdir = argv[5];
        std::thread a(run_instance, 0, std::string(argv[2]), std::string(argv[3]), std::string());
        std::thread b(run_instance, 1, std::string(argv[2]), std::string(argv[4]), std::string());
        a.join();
        b.join();
    }
    return 0;
}
