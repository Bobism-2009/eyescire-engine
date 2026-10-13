// =============================================================================
// Visual blueprints.
//
// A blueprint is a binary graph (EBP1) the editor draws. Play and Build
// translate it into scripts/<name>.nxa, which then compiles like any other
// script. The generated file starts with "// EYESCIRE_BLUEPRINT" so a
// hand-written .nxa of the same name is never overwritten.
//
// Kinds:
//   1 Init   2 Update  3 Branch  4 Sequence  5 Log
//   6 Get Position  7 Set Position  8 Key Pressed
//   9 Spawn  10 Destroy  11 Math  12 Object
//   13 Function entry  14 Return  15 Call
//   16 Get Value  17 Set Value  18 If
//   19 Set Velocity  20 Get Velocity  21 Mouse
//   22 Overlap  23 Spawn Actor  24 Set Yaw  25 Get Yaw
//   33 Delta Time  34 Play Sound  35 Play Music  36 Stop Music
//   37 Apply Force  38 Apply Impulse  39 Set Visible
//   40 Set Color  41 Set Size  42 Set Text  43 Load Scene
//   44 Tween Pose  45 Tween Fade
// If stores the comparison in op: 0 equal, 1 not equal, 2 greater,
// 3 less, 4 at least, 5 at most. Pins are exec in, value, is, true, false.
// Custom functions, then custom values, are stored after the version-1
// graph. Older builds still open the event graph and ignore that tail.
// Pin types: 0 exec, 1 bool, 2 float, 3 string, 4 entity.
// Pin dir: 0 in, 1 out.
// A call or entry stores its argument count in op (0..4).
// =============================================================================

namespace {

enum BpKind {
    BP_INIT = 1,
    BP_UPDATE = 2,
    BP_BRANCH = 3,
    BP_SEQUENCE = 4,
    BP_LOG = 5,
    BP_GET = 6,
    BP_SET = 7,
    BP_KEY = 8,
    BP_SPAWN = 9,
    BP_DESTROY = 10,
    BP_MATH = 11,
    BP_OBJECT = 12,
    BP_ENTRY = 13,
    BP_RETURN = 14,
    BP_CALL = 15,
    BP_GETVAR = 16,
    BP_SETVAR = 17,
    BP_IF = 18,
    BP_VEL = 19,
    BP_GETVEL = 20,
    BP_MOUSE = 21,
    BP_HIT = 22,
    BP_SPAWN3 = 23,
    BP_YAW = 24,
    BP_GETYAW = 25,
    BP_AXIS = 26,
    BP_LOOK = 27,
    BP_PITCH = 28,
    BP_GETPITCH = 29,
    BP_VIEW = 30,
    BP_WALK = 31,
    BP_NUDGE = 32,
    BP_DT = 33,
    BP_SFX = 34,
    BP_MUSIC = 35,
    BP_MUSIC_STOP = 36,
    BP_FORCE = 37,
    BP_IMPULSE = 38,
    BP_VISIBLE = 39,
    BP_COLOR = 40,
    BP_SIZE = 41,
    BP_SETTEXT = 42,
    BP_SCENE = 43,
    BP_TWEEN = 44,
    BP_FADE = 45
};

enum BpType { BP_EXEC = 0, BP_BOOL = 1, BP_FLOAT = 2, BP_STRING = 3, BP_ENTITY = 4 };

struct PinDef {
    int dir;
    int type;
    const char* name;
};

struct BpNode {
    int kind = 0;
    int x = 0;
    int y = 0;
    int op = 0;
    float a = 0.f;
    float b = 0.f;
    std::string text;
    std::string name;
};

struct BpLink {
    int from_node = 0;
    int from_pin = 0;
    int to_node = 0;
    int to_pin = 0;
};

struct BpGraph {
    int pan_x = 40;
    int pan_y = 40;
    std::vector<BpNode> nodes;
    std::vector<BpLink> links;
};

struct BpFn {
    std::string name;
    int argc = 0;
    std::string argn[4];
    BpGraph g;
};

struct BpVar {
    std::string name;
    int type = 2; /* bool 1, float 2, string 3 */
    float num = 0.f;
    std::string text;
};

struct BpDoc {
    bool open = false;
    bool dirty = false;
    std::string path;
    int pan_x = 0;
    int pan_y = 0;
    std::vector<BpNode> nodes;
    std::vector<BpLink> links;
    BpGraph event;
    std::vector<BpFn> fns;
    std::vector<BpVar> vars;
    int edit_fn = -1;
};

struct BpHeld {
    bool hasF = false;
    float f = 0.f;
    bool hasB = false;
    int b = 0;
    bool hasS = false;
    std::string s;
};

std::unordered_map<int, std::vector<BpHeld>> g_bpHeld;
int g_bpShareKey = 0;
std::vector<int> g_bpShareStack;

BpDoc g_bp;
std::string g_bpRet;

struct PinSet {
    const PinDef* pins;
    int count;
};

PinSet pins_for(int kind) {
    static const PinDef init[] = { { 1, BP_EXEC, "then" } };
    static const PinDef update[] = { { 1, BP_EXEC, "then" } };
    static const PinDef branch[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_BOOL, "if" },
        { 1, BP_EXEC, "true" },
        { 1, BP_EXEC, "false" }
    };
    static const PinDef sequence[] = {
        { 0, BP_EXEC, "" },
        { 1, BP_EXEC, "first" },
        { 1, BP_EXEC, "second" }
    };
    static const PinDef logn[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_STRING, "text" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef getp[] = {
        { 1, BP_FLOAT, "x" },
        { 1, BP_FLOAT, "y" },
        { 1, BP_FLOAT, "z" }
    };
    static const PinDef setp[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef key[] = { { 1, BP_BOOL, "pressed" } };
    static const PinDef spawn[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 1, BP_EXEC, "then" },
        { 1, BP_ENTITY, "entity" }
    };
    static const PinDef destroy[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_ENTITY, "entity" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef math[] = {
        { 0, BP_FLOAT, "a" },
        { 0, BP_FLOAT, "b" },
        { 1, BP_FLOAT, "result" }
    };
    /* Dropped from the hierarchy. op is the entity id. a/b are x/y.
       text is "z rot" for the other two defaults. */
    static const PinDef object[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "set x" },
        { 0, BP_FLOAT, "set y" },
        { 0, BP_FLOAT, "set z" },
        { 0, BP_FLOAT, "set rot" },
        { 1, BP_EXEC, "then" },
        { 1, BP_FLOAT, "x" },
        { 1, BP_FLOAT, "y" },
        { 1, BP_FLOAT, "z" },
        { 1, BP_FLOAT, "rot" },
        { 1, BP_ENTITY, "entity" },
        { 1, BP_FLOAT, "yaw" },
        { 0, BP_FLOAT, "set yaw" },
        { 0, BP_FLOAT, "set pitch" }
    };
    static const PinDef vel[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "vx" },
        { 0, BP_FLOAT, "vy" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef getvel[] = {
        { 1, BP_FLOAT, "vx" },
        { 1, BP_FLOAT, "vy" }
    };
    static const PinDef mouse[] = { { 1, BP_BOOL, "down" } };
    static const PinDef hit[] = {
        { 0, BP_EXEC, "" },
        { 1, BP_EXEC, "hit" },
        { 1, BP_EXEC, "miss" },
        { 1, BP_ENTITY, "entity" }
    };
    static const PinDef spawn3[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 0, BP_FLOAT, "z" },
        { 0, BP_FLOAT, "yaw" },
        { 1, BP_EXEC, "then" },
        { 1, BP_ENTITY, "entity" },
        { 0, BP_FLOAT, "pitch" }
    };
    static const PinDef yaw[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "yaw" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef getyaw[] = { { 1, BP_FLOAT, "yaw" } };
    static const PinDef axis[] = { { 1, BP_FLOAT, "axis" } };
    static const PinDef look[] = {
        { 0, BP_EXEC, "" },
        { 1, BP_EXEC, "then" },
        { 1, BP_FLOAT, "yaw" },
        { 1, BP_FLOAT, "pitch" }
    };
    static const PinDef pitch[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "pitch" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef getpitch[] = { { 1, BP_FLOAT, "pitch" } };
    static const PinDef view[] = {
        { 0, BP_FLOAT, "yaw" },
        { 0, BP_FLOAT, "pitch" },
        { 0, BP_FLOAT, "forward" },
        { 0, BP_FLOAT, "right" },
        { 0, BP_FLOAT, "down" },
        { 1, BP_FLOAT, "x" },
        { 1, BP_FLOAT, "y" },
        { 1, BP_FLOAT, "z" }
    };
    static const PinDef walk[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "yaw" },
        { 0, BP_FLOAT, "forward" },
        { 0, BP_FLOAT, "strafe" },
        { 0, BP_BOOL, "jump" },
        { 1, BP_EXEC, "then" },
        { 1, BP_BOOL, "ground" }
    };
    static const PinDef nudge[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 0, BP_FLOAT, "z" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef dt[] = { { 1, BP_FLOAT, "dt" } };
    static const PinDef sfx[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_STRING, "path" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef music[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_STRING, "path" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef music_stop[] = {
        { 0, BP_EXEC, "" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef force[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef impulse[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef visible[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_BOOL, "on" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef color[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "r" },
        { 0, BP_FLOAT, "g" },
        { 0, BP_FLOAT, "b" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef size[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "w" },
        { 0, BP_FLOAT, "h" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef settext[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_STRING, "text" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef scene[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_STRING, "path" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef tween[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "x" },
        { 0, BP_FLOAT, "y" },
        { 0, BP_FLOAT, "z" },
        { 0, BP_FLOAT, "rot" },
        { 0, BP_FLOAT, "sec" },
        { 1, BP_EXEC, "then" }
    };
    static const PinDef fade[] = {
        { 0, BP_EXEC, "" },
        { 0, BP_FLOAT, "alpha" },
        { 0, BP_FLOAT, "sec" },
        { 1, BP_EXEC, "then" }
    };
    switch (kind) {
    case BP_INIT:       return { init, 1 };
    case BP_UPDATE:     return { update, 1 };
    case BP_BRANCH:     return { branch, 4 };
    case BP_SEQUENCE:   return { sequence, 3 };
    case BP_LOG:        return { logn, 3 };
    case BP_GET:        return { getp, 3 };
    case BP_SET:        return { setp, 4 };
    case BP_KEY:        return { key, 1 };
    case BP_SPAWN:      return { spawn, 5 };
    case BP_DESTROY:    return { destroy, 3 };
    case BP_MATH:       return { math, 3 };
    case BP_OBJECT:     return { object, 14 };
    case BP_VEL:        return { vel, 4 };
    case BP_GETVEL:     return { getvel, 2 };
    case BP_MOUSE:      return { mouse, 1 };
    case BP_HIT:        return { hit, 4 };
    case BP_SPAWN3:     return { spawn3, 8 };
    case BP_YAW:        return { yaw, 3 };
    case BP_GETYAW:     return { getyaw, 1 };
    case BP_AXIS:       return { axis, 1 };
    case BP_LOOK:       return { look, 4 };
    case BP_PITCH:      return { pitch, 3 };
    case BP_GETPITCH:   return { getpitch, 1 };
    case BP_VIEW:       return { view, 8 };
    case BP_WALK:       return { walk, 7 };
    case BP_NUDGE:      return { nudge, 5 };
    case BP_DT:         return { dt, 1 };
    case BP_SFX:        return { sfx, 3 };
    case BP_MUSIC:      return { music, 3 };
    case BP_MUSIC_STOP: return { music_stop, 2 };
    case BP_FORCE:      return { force, 4 };
    case BP_IMPULSE:    return { impulse, 4 };
    case BP_VISIBLE:    return { visible, 3 };
    case BP_COLOR:      return { color, 5 };
    case BP_SIZE:       return { size, 4 };
    case BP_SETTEXT:    return { settext, 3 };
    case BP_SCENE:      return { scene, 3 };
    case BP_TWEEN:      return { tween, 7 };
    case BP_FADE:       return { fade, 4 };
    default:            return { nullptr, 0 };
    }
}

const char* kind_name_of(int kind) {
    switch (kind) {
    case BP_INIT:       return "Init";
    case BP_UPDATE:     return "Update";
    case BP_BRANCH:     return "Branch";
    case BP_SEQUENCE:   return "Sequence";
    case BP_LOG:        return "Log";
    case BP_GET:        return "Get Position";
    case BP_SET:        return "Set Position";
    case BP_KEY:        return "Key Pressed";
    case BP_SPAWN:      return "Spawn";
    case BP_DESTROY:    return "Destroy";
    case BP_MATH:       return "Math";
    case BP_OBJECT:     return "Object";
    case BP_ENTRY:      return "Function";
    case BP_RETURN:     return "Return";
    case BP_CALL:       return "Call";
    case BP_GETVAR:     return "Get Value";
    case BP_SETVAR:     return "Set Value";
    case BP_IF:         return "If";
    case BP_VEL:        return "Set Velocity";
    case BP_GETVEL:     return "Get Velocity";
    case BP_MOUSE:      return "Mouse";
    case BP_HIT:        return "Overlap";
    case BP_SPAWN3:     return "Spawn Actor";
    case BP_YAW:        return "Set Yaw";
    case BP_GETYAW:     return "Get Yaw";
    case BP_AXIS:       return "Axis";
    case BP_LOOK:       return "Mouse Look";
    case BP_PITCH:      return "Set Pitch";
    case BP_GETPITCH:   return "Get Pitch";
    case BP_VIEW:       return "View Offset";
    case BP_WALK:       return "Walk";
    case BP_NUDGE:      return "Move By";
    case BP_DT:         return "Delta Time";
    case BP_SFX:        return "Play Sound";
    case BP_MUSIC:      return "Play Music";
    case BP_MUSIC_STOP: return "Stop Music";
    case BP_FORCE:      return "Apply Force";
    case BP_IMPULSE:    return "Apply Impulse";
    case BP_VISIBLE:    return "Set Visible";
    case BP_COLOR:      return "Set Color";
    case BP_SIZE:       return "Set Size";
    case BP_SETTEXT:    return "Set Text";
    case BP_SCENE:      return "Load Scene";
    case BP_TWEEN:      return "Tween Pose";
    case BP_FADE:       return "Tween Fade";
    default:            return "";
    }
}

struct PinInfo {
    bool ok = false;
    int dir = 0;
    int type = 0;
    const char* name = "";
};

int clamp_argc(int a) {
    if (a < 0) return 0;
    if (a > 4) return 4;
    return a;
}

int clamp_vtype(int t) {
    if (t == BP_BOOL || t == BP_STRING) return t;
    return BP_FLOAT;
}

const char* vtype_pin_name(int t) {
    if (t == BP_BOOL) return "bool";
    if (t == BP_STRING) return "text";
    return "float";
}

const PinDef* pin_def(int kind, int pin);

PinInfo pin_info(const BpNode& n, int pin) {
    if (n.kind == BP_ENTRY) {
        int argc = clamp_argc(n.op);
        if (pin == 0) return { true, 1, BP_EXEC, "then" };
        if (pin >= 1 && pin <= argc) return { true, 1, BP_FLOAT, "" };
        return {};
    }
    if (n.kind == BP_RETURN) {
        if (pin == 0) return { true, 0, BP_EXEC, "" };
        if (pin == 1) return { true, 0, BP_FLOAT, "result" };
        return {};
    }
    if (n.kind == BP_CALL) {
        int argc = clamp_argc(n.op);
        if (pin == 0) return { true, 0, BP_EXEC, "" };
        if (pin >= 1 && pin <= argc) return { true, 0, BP_FLOAT, "" };
        if (pin == argc + 1) return { true, 1, BP_EXEC, "then" };
        if (pin == argc + 2) return { true, 1, BP_FLOAT, "result" };
        return {};
    }
    if (n.kind == BP_GETVAR) {
        if (pin == 0) return { true, 1, clamp_vtype(n.op), vtype_pin_name(clamp_vtype(n.op)) };
        return {};
    }
    if (n.kind == BP_SETVAR) {
        if (pin == 0) return { true, 0, BP_EXEC, "" };
        if (pin == 1) return { true, 0, clamp_vtype(n.op), "value" };
        if (pin == 2) return { true, 1, BP_EXEC, "then" };
        return {};
    }
    if (n.kind == BP_IF) {
        if (pin == 0) return { true, 0, BP_EXEC, "" };
        if (pin == 1) return { true, 0, BP_FLOAT, "value" };
        if (pin == 2) {
            const char* name = "is";
            if (n.op == 1) name = "is not";
            else if (n.op == 2 || n.op == 3 || n.op == 4 || n.op == 5) name = "than";
            return { true, 0, BP_FLOAT, name };
        }
        if (pin == 3) return { true, 1, BP_EXEC, "true" };
        if (pin == 4) return { true, 1, BP_EXEC, "false" };
        return {};
    }
    const PinDef* p = pin_def(n.kind, pin);
    if (!p) return {};
    return { true, p->dir, p->type, p->name ? p->name : "" };
}

int pin_count_of(const BpNode& n) {
    if (n.kind == BP_ENTRY) return 1 + clamp_argc(n.op);
    if (n.kind == BP_RETURN) return 2;
    if (n.kind == BP_CALL) return 3 + clamp_argc(n.op);
    if (n.kind == BP_GETVAR) return 1;
    if (n.kind == BP_SETVAR) return 3;
    if (n.kind == BP_IF) return 5;
    return pins_for(n.kind).count;
}

const PinDef* pin_def(int kind, int pin) {
    PinSet s = pins_for(kind);
    if (pin < 0 || pin >= s.count) return nullptr;
    return &s.pins[pin];
}

bool eq_i(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

bool ends_with_i(const std::string& s, const std::string& suf) {
    if (suf.size() > s.size()) return false;
    return eq_i(s.substr(s.size() - suf.size()), suf);
}

std::string stem_of(const std::string& filename) {
    std::string s = filename;
    auto slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    auto dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

/* Keep the folder. "ui/mover.nxa" + ".ebp" -> "ui/mover.ebp". */
std::string rel_with_ext(const std::string& rel, const char* ext) {
    std::string s = rel;
    for (char& c : s) if (c == '\\') c = '/';
    auto slash = s.find_last_of('/');
    auto dot = s.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        s.resize(dot);
    return s + (ext ? ext : "");
}

std::string rel_key(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

std::string file_name_of(const std::string& path) {
    auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string norm_path(const std::string& p) {
    std::error_code ec;
    fs::path a = fs::absolute(fs::path(p), ec);
    std::string s = ec ? p : a.lexically_normal().string();
    for (char& c : s) {
        if (c == '/') c = '\\';
        else c = (char)tolower((unsigned char)c);
    }
    return s;
}

bool same_path(const std::string& a, const std::string& b) {
    return norm_path(a) == norm_path(b);
}

std::string nxa_for(const std::string& ebp) {
    if (ends_with_i(ebp, ".ebp")) return ebp.substr(0, ebp.size() - 4) + ".nxa";
    return ebp + ".nxa";
}

bool path_is_extension_ebp(const std::string& p) {
    std::string n = norm_path(p);
    for (char& c : n) if (c == '\\') c = '/';
    return n.find("/extensions/") != std::string::npos;
}

std::string read_bin(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string read_all(const std::string& path) {
    std::string s = read_bin(path);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

bool text_is_generated(const std::string& s) {
    return s.rfind("// EYESCIRE_BLUEPRINT", 0) == 0;
}

void w_u32(std::ostream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void w_i32(std::ostream& o, int32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void w_f32(std::ostream& o, float v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void w_str(std::ostream& o, const std::string& s) {
    uint32_t n = (uint32_t)s.size();
    if (n > 4096) n = 4096;
    w_u32(o, n);
    if (n) o.write(s.data(), n);
}

struct Rdr {
    const char* p = nullptr;
    const char* end = nullptr;
    bool ok = true;
    uint32_t u32() {
        if (!ok || p + 4 > end) { ok = false; return 0; }
        uint32_t v = 0;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    float f32() {
        uint32_t u = u32();
        float f = 0.f;
        memcpy(&f, &u, 4);
        return f;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || n > 4096 || p + n > end) { ok = false; return ""; }
        std::string s(p, p + n);
        p += n;
        return s;
    }
};

std::string nxa_float(float v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", (double)v);
    std::string s = buf;
    while (s.size() > 2 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
    return s;
}

std::string nxa_str(const std::string& in) {
    std::string o = "\"";
    for (unsigned char c : in) {
        if (c == '\\' || c == '"') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c >= 32) o += (char)c;
    }
    o += '"';
    return o;
}

std::string key_label_s(int op) {
    if (op == 32) return "Space";
    if (op == 37) return "Left";
    if (op == 38) return "Up";
    if (op == 39) return "Right";
    if (op == 40) return "Down";
    if (op == 27) return "Esc";
    if (op == 16) return "Shift";
    if (op == 17) return "Ctrl";
    if (op == 13) return "Enter";
    if (op == 9) return "Tab";
    if (op >= 48 && op <= 57) return std::string(1, (char)op);
    if (op >= 65 && op <= 90) return std::string(1, (char)op);
    return std::to_string(op);
}

int parse_key(const char* s) {
    if (!s) return -1;
    std::string t;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') continue;
        t += (char)tolower(c);
    }
    if (t.empty()) return -1;
    if (t == "space") return 32;
    if (t == "left") return 37;
    if (t == "up") return 38;
    if (t == "right") return 39;
    if (t == "down") return 40;
    if (t == "esc" || t == "escape") return 27;
    if (t == "shift") return 16;
    if (t == "ctrl" || t == "control") return 17;
    if (t == "enter" || t == "return") return 13;
    if (t == "tab") return 9;
    if (t.size() == 1 && t[0] >= 'a' && t[0] <= 'z') return t[0] - 'a' + 'A';
    if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') return (unsigned char)t[0];
    bool dig = true;
    for (char c : t) if (c < '0' || c > '9') dig = false;
    if (dig) {
        int v = atoi(t.c_str());
        if (v >= 1 && v <= 254) return v;
    }
    return -1;
}

bool parse_f(const char* s, float* out) {
    if (!s || !out) return false;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return false;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (end == s) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != 0) return false;
    if (!std::isfinite(v)) return false;
    *out = v;
    return true;
}

std::string sanitize_base(const char* name) {
    std::string in = name ? name : "";
    auto slash = in.find_last_of("\\/");
    if (slash != std::string::npos) in = in.substr(slash + 1);
    auto dot = in.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = in.substr(dot);
        if (eq_i(ext, ".ebp") || eq_i(ext, ".nxa")) in = in.substr(0, dot);
    }
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '_') out += (char)c;
    }
    if (out.empty()) return "";
    unsigned char c0 = (unsigned char)out[0];
    if (!isalpha(c0) && c0 != '_') out = "bp_" + out;
    return out;
}

void default_nodes(BpDoc& d) {
    d.nodes.clear();
    d.links.clear();
    d.fns.clear();
    d.vars.clear();
    d.edit_fn = -1;
    d.pan_x = 40;
    d.pan_y = 40;
    BpNode init;
    init.kind = BP_INIT;
    init.x = 40;
    init.y = 40;
    BpNode update;
    update.kind = BP_UPDATE;
    update.x = 40;
    update.y = 240;
    d.nodes.push_back(init);
    d.nodes.push_back(update);
    d.event.pan_x = d.pan_x;
    d.event.pan_y = d.pan_y;
    d.event.nodes = d.nodes;
    d.event.links = d.links;
}

int find_kind(const BpDoc& d, int kind) {
    for (int i = 0; i < (int)d.nodes.size(); ++i)
        if (d.nodes[i].kind == kind) return i;
    return -1;
}

bool link_types_ok(const BpDoc& d, int fn, int fp, int tn, int tp) {
    if (fn < 0 || tn < 0 || fn >= (int)d.nodes.size() || tn >= (int)d.nodes.size()) return false;
    if (fn == tn) return false;
    PinInfo a = pin_info(d.nodes[fn], fp);
    PinInfo b = pin_info(d.nodes[tn], tp);
    if (!a.ok || !b.ok) return false;
    if (a.dir == 0 && b.dir == 1) std::swap(a, b);
    if (a.dir != 1 || b.dir != 0) return false;
    if (a.type != b.type) return false;
    return true;
}

void orient_link(const BpDoc& d, int& fn, int& fp, int& tn, int& tp) {
    PinInfo a = pin_info(d.nodes[fn], fp);
    PinInfo b = pin_info(d.nodes[tn], tp);
    if (a.ok && b.ok && a.dir == 0 && b.dir == 1) {
        std::swap(fn, tn);
        std::swap(fp, tp);
    }
}

bool input_taken(const BpDoc& d, int node, int pin) {
    for (const BpLink& L : d.links)
        if (L.to_node == node && L.to_pin == pin) return true;
    return false;
}

bool exec_out_taken(const BpDoc& d, int node, int pin) {
    PinInfo p = pin_info(d.nodes[node], pin);
    if (!p.ok || p.type != BP_EXEC || p.dir != 1) return false;
    for (const BpLink& L : d.links)
        if (L.from_node == node && L.from_pin == pin) return true;
    return false;
}

static const char* kArgFallback[4] = { "a", "b", "c", "d" };

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s = s.substr(i);
    return s;
}

std::string clean_fn_name(const std::string& in) {
    std::string s;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '_' || c == ' ') s += (char)c;
        if (s.size() >= 40) break;
    }
    return trim_copy(s);
}

std::string clean_arg_name(const std::string& in) {
    std::string s;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '_' || c == ' ') s += (char)c;
        if (s.size() >= 16) break;
    }
    return trim_copy(s);
}

int find_fn_index(const BpDoc& d, const std::string& name) {
    for (int i = 0; i < (int)d.fns.size(); ++i)
        if (eq_i(d.fns[i].name, name)) return i;
    return -1;
}

bool fn_name_taken(const BpDoc& d, const std::string& name, int except) {
    for (int i = 0; i < (int)d.fns.size(); ++i) {
        if (i == except) continue;
        if (eq_i(d.fns[i].name, name)) return true;
    }
    return false;
}

std::string unique_fn_name(const BpDoc& d, const std::string& base, int except) {
    std::string b = base.empty() ? "Function" : base;
    if (!fn_name_taken(d, b, except)) return b;
    for (int n = 2; n < 1000; ++n) {
        std::string c = b + std::to_string(n);
        if (!fn_name_taken(d, c, except)) return c;
    }
    return b;
}

int find_var_index(const BpDoc& d, const std::string& name) {
    for (int i = 0; i < (int)d.vars.size(); ++i)
        if (eq_i(d.vars[i].name, name)) return i;
    return -1;
}

bool var_name_taken(const BpDoc& d, const std::string& name, int except) {
    for (int i = 0; i < (int)d.vars.size(); ++i) {
        if (i == except) continue;
        if (eq_i(d.vars[i].name, name)) return true;
    }
    return false;
}

std::string unique_var_name(const BpDoc& d, const std::string& base, int except) {
    std::string b = base.empty() ? "Value" : base;
    if (!var_name_taken(d, b, except)) return b;
    for (int n = 2; n < 1000; ++n) {
        std::string c = b + std::to_string(n);
        if (!var_name_taken(d, c, except)) return c;
    }
    return b;
}

std::string arg_name_of(const BpDoc& d, const BpNode& n, int arg) {
    if (arg < 0 || arg > 3) return "";
    const BpFn* f = nullptr;
    if (n.kind == BP_ENTRY && d.edit_fn >= 0 && d.edit_fn < (int)d.fns.size())
        f = &d.fns[d.edit_fn];
    if (n.kind == BP_CALL) {
        int i = find_fn_index(d, n.name);
        if (i >= 0) f = &d.fns[i];
    }
    if (f && !f->argn[arg].empty()) return f->argn[arg];
    return kArgFallback[arg];
}

void flush_active(BpDoc& d) {
    if (d.edit_fn >= 0 && d.edit_fn < (int)d.fns.size()) {
        BpGraph& g = d.fns[d.edit_fn].g;
        g.pan_x = d.pan_x;
        g.pan_y = d.pan_y;
        g.nodes = d.nodes;
        g.links = d.links;
    } else {
        d.edit_fn = -1;
        d.event.pan_x = d.pan_x;
        d.event.pan_y = d.pan_y;
        d.event.nodes = d.nodes;
        d.event.links = d.links;
    }
}

void apply_graph(BpDoc& d, const BpGraph& g) {
    d.pan_x = g.pan_x;
    d.pan_y = g.pan_y;
    d.nodes = g.nodes;
    d.links = g.links;
}

void reload_active(BpDoc& d) {
    if (d.edit_fn >= 0 && d.edit_fn < (int)d.fns.size()) apply_graph(d, d.fns[d.edit_fn].g);
    else {
        d.edit_fn = -1;
        apply_graph(d, d.event);
    }
}

void show_edit(BpDoc& d, int idx) {
    flush_active(d);
    if (idx < 0 || idx >= (int)d.fns.size()) idx = -1;
    d.edit_fn = idx;
    reload_active(d);
}

void erase_node_at(std::vector<BpNode>& nodes, std::vector<BpLink>& links, int i) {
    if (i < 0 || i >= (int)nodes.size()) return;
    links.erase(std::remove_if(links.begin(), links.end(), [&](const BpLink& L) {
        return L.from_node == i || L.to_node == i;
    }), links.end());
    for (BpLink& L : links) {
        if (L.from_node > i) L.from_node--;
        if (L.to_node > i) L.to_node--;
    }
    nodes.erase(nodes.begin() + i);
}

void remap_node_links(std::vector<BpLink>& links, int node, int kind, int oldA, int newA) {
    oldA = clamp_argc(oldA);
    newA = clamp_argc(newA);
    for (auto it = links.begin(); it != links.end();) {
        bool from = it->from_node == node;
        bool to = it->to_node == node;
        if (!from && !to) { ++it; continue; }
        int pin = from ? it->from_pin : it->to_pin;
        int mapped = pin;
        bool drop = false;
        if (kind == BP_ENTRY) {
            if (pin == 0) mapped = 0;
            else if (pin >= 1 && pin <= newA) mapped = pin;
            else drop = true;
        } else if (kind == BP_CALL) {
            if (pin == 0) mapped = 0;
            else if (pin >= 1 && pin <= oldA) {
                if (pin <= newA) mapped = pin;
                else drop = true;
            } else if (pin == oldA + 1) mapped = newA + 1;
            else if (pin == oldA + 2) mapped = newA + 2;
            else drop = true;
        }
        if (drop) it = links.erase(it);
        else {
            if (from) it->from_pin = mapped;
            if (to) it->to_pin = mapped;
            ++it;
        }
    }
}

void prune_bad_links(std::vector<BpNode>& nodes, std::vector<BpLink>& links) {
    BpDoc tmp;
    tmp.nodes = nodes;
    for (const BpLink& in : links) {
        BpLink L = in;
        if (!link_types_ok(tmp, L.from_node, L.from_pin, L.to_node, L.to_pin)) continue;
        orient_link(tmp, L.from_node, L.from_pin, L.to_node, L.to_pin);
        if (input_taken(tmp, L.to_node, L.to_pin)) continue;
        if (exec_out_taken(tmp, L.from_node, L.from_pin)) continue;
        tmp.links.push_back(L);
    }
    links = std::move(tmp.links);
}

void strip_var_nodes(std::vector<BpNode>& nodes, std::vector<BpLink>& links, const std::string& name) {
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        int k = nodes[i].kind;
        if ((k == BP_GETVAR || k == BP_SETVAR) && eq_i(nodes[i].name, name))
            erase_node_at(nodes, links, i);
    }
}

void rename_var_nodes(std::vector<BpNode>& nodes, const std::string& oldName, const std::string& newName) {
    for (BpNode& n : nodes) {
        if ((n.kind == BP_GETVAR || n.kind == BP_SETVAR) && eq_i(n.name, oldName))
            n.name = newName;
    }
}

void retype_var_nodes(std::vector<BpNode>& nodes, std::vector<BpLink>& links, const std::string& name, int type) {
    bool changed = false;
    for (BpNode& n : nodes) {
        if ((n.kind != BP_GETVAR && n.kind != BP_SETVAR) || !eq_i(n.name, name)) continue;
        if (n.op != type) { n.op = type; changed = true; }
    }
    if (changed) prune_bad_links(nodes, links);
}

void ensure_entry(BpFn& f) {
    int first = -1;
    for (int i = 0; i < (int)f.g.nodes.size(); ++i) {
        if (f.g.nodes[i].kind == BP_ENTRY) { first = i; break; }
    }
    if (first >= 0) {
        for (int i = (int)f.g.nodes.size() - 1; i > first; --i) {
            if (f.g.nodes[i].kind == BP_ENTRY)
                erase_node_at(f.g.nodes, f.g.links, i);
        }
    } else {
        BpNode e;
        e.kind = BP_ENTRY;
        e.x = 40;
        e.y = 40;
        f.g.nodes.insert(f.g.nodes.begin(), e);
        for (BpLink& L : f.g.links) {
            if (L.from_node >= 0) L.from_node++;
            if (L.to_node >= 0) L.to_node++;
        }
        first = 0;
    }
    f.g.nodes[first].kind = BP_ENTRY;
    f.g.nodes[first].op = f.argc;
    f.g.nodes[first].name = f.name;
}

void sync_sig_nodes(BpDoc& d, std::vector<BpNode>& nodes, std::vector<BpLink>& links, int entryFn) {
    for (int i = 0; i < (int)nodes.size(); ++i) {
        BpNode& n = nodes[i];
        if (entryFn >= 0 && n.kind == BP_ENTRY) {
            int want = d.fns[entryFn].argc;
            int old = clamp_argc(n.op);
            if (old != want) remap_node_links(links, i, BP_ENTRY, old, want);
            n.op = want;
            n.name = d.fns[entryFn].name;
        } else if (n.kind == BP_CALL) {
            int fi = find_fn_index(d, n.name);
            if (fi < 0) continue;
            int want = d.fns[fi].argc;
            int old = clamp_argc(n.op);
            if (old != want) remap_node_links(links, i, BP_CALL, old, want);
            n.op = want;
        }
    }
    prune_bad_links(nodes, links);
}

void retarget_name(std::vector<BpNode>& nodes, const std::string& oldName, const std::string& newName, bool entryHere) {
    for (BpNode& n : nodes) {
        if (entryHere && n.kind == BP_ENTRY) n.name = newName;
        else if (n.kind == BP_CALL && eq_i(n.name, oldName)) n.name = newName;
    }
}

void strip_calls(std::vector<BpNode>& nodes, std::vector<BpLink>& links, const std::string& name) {
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        if (nodes[i].kind == BP_CALL && eq_i(nodes[i].name, name))
            erase_node_at(nodes, links, i);
    }
}

std::string fn_ident(const BpDoc& d, int index) {
    std::string body;
    if (index >= 0 && index < (int)d.fns.size()) {
        for (unsigned char c : d.fns[index].name) {
            if (isalnum(c) || c == '_') body += (char)c;
            else body += '_';
        }
    }
    if (body.empty()) body = "fn";
    return "bpfn_" + std::to_string(index) + "_" + body;
}

void write_nodes(std::ostream& o, const std::vector<BpNode>& nodes) {
    w_u32(o, (uint32_t)nodes.size());
    for (const BpNode& n : nodes) {
        w_u32(o, (uint32_t)n.kind);
        w_i32(o, n.x);
        w_i32(o, n.y);
        w_i32(o, n.op);
        w_f32(o, n.a);
        w_f32(o, n.b);
        w_str(o, n.text);
        w_str(o, n.name);
    }
}

void write_links(std::ostream& o, const std::vector<BpLink>& links) {
    w_u32(o, (uint32_t)links.size());
    for (const BpLink& L : links) {
        w_u32(o, (uint32_t)L.from_node);
        w_u32(o, (uint32_t)L.from_pin);
        w_u32(o, (uint32_t)L.to_node);
        w_u32(o, (uint32_t)L.to_pin);
    }
}

bool read_node(Rdr& r, BpNode& n) {
    n.kind = (int)r.u32();
    n.x = r.i32();
    n.y = r.i32();
    n.op = r.i32();
    n.a = r.f32();
    n.b = r.f32();
    n.text = r.str();
    n.name = r.str();
    if (!r.ok) return false;
    if (n.kind < BP_INIT || n.kind > BP_FADE) return false;
    if (n.kind == BP_KEY && (n.op < 1 || n.op > 254)) n.op = 32;
    if (n.kind == BP_MATH && (n.op < 0 || n.op > 3)) n.op = 0;
    if (n.kind == BP_IF && (n.op < 0 || n.op > 5)) n.op = 0;
    if (n.kind == BP_ENTRY || n.kind == BP_CALL) n.op = clamp_argc(n.op);
    if (n.kind == BP_GETVAR || n.kind == BP_SETVAR) n.op = clamp_vtype(n.op);
    return true;
}

bool read_nodes(Rdr& r, std::vector<BpNode>& nodes) {
    uint32_t nc = r.u32();
    if (!r.ok || nc > 512) return false;
    nodes.clear();
    nodes.reserve(nc);
    for (uint32_t i = 0; i < nc; ++i) {
        BpNode n;
        if (!read_node(r, n)) return false;
        nodes.push_back(std::move(n));
    }
    return true;
}

bool read_links_into(Rdr& r, std::vector<BpNode>& nodes, std::vector<BpLink>& links) {
    uint32_t lc = r.u32();
    if (!r.ok || lc > 1024) return false;
    BpDoc tmp;
    tmp.nodes = nodes;
    for (uint32_t i = 0; i < lc; ++i) {
        BpLink L;
        L.from_node = (int)r.u32();
        L.from_pin = (int)r.u32();
        L.to_node = (int)r.u32();
        L.to_pin = (int)r.u32();
        if (!r.ok) return false;
        if (!link_types_ok(tmp, L.from_node, L.from_pin, L.to_node, L.to_pin)) continue;
        orient_link(tmp, L.from_node, L.from_pin, L.to_node, L.to_pin);
        if (input_taken(tmp, L.to_node, L.to_pin)) continue;
        if (exec_out_taken(tmp, L.from_node, L.from_pin)) continue;
        tmp.links.push_back(L);
    }
    links = std::move(tmp.links);
    return true;
}

bool save_doc(BpDoc& d) {
    if (d.path.empty()) return 0;
    flush_active(d);
    std::ofstream o(d.path, std::ios::binary | std::ios::trunc);
    if (!o) return 0;
    o.write("EBP1", 4);
    w_u32(o, 1);
    w_i32(o, d.event.pan_x);
    w_i32(o, d.event.pan_y);
    write_nodes(o, d.event.nodes);
    write_links(o, d.event.links);
    w_u32(o, (uint32_t)d.fns.size());
    for (const BpFn& f : d.fns) {
        w_str(o, f.name);
        w_i32(o, f.argc);
        for (int i = 0; i < 4; ++i) w_str(o, f.argn[i]);
        w_i32(o, f.g.pan_x);
        w_i32(o, f.g.pan_y);
        write_nodes(o, f.g.nodes);
        write_links(o, f.g.links);
    }
    w_u32(o, (uint32_t)d.vars.size());
    for (const BpVar& v : d.vars) {
        w_str(o, v.name);
        w_i32(o, v.type);
        w_f32(o, v.num);
        w_str(o, v.text);
    }
    if (!o) return 0;
    d.dirty = false;
    return 1;
}

bool load_fn_tail(Rdr& r, BpDoc& d) {
    if (r.p >= r.end || !r.ok) return true;
    uint32_t fc = r.u32();
    if (!r.ok || fc > 64) return true;
    for (uint32_t i = 0; i < fc; ++i) {
        BpFn f;
        f.name = r.str();
        f.argc = clamp_argc(r.i32());
        for (int a = 0; a < 4; ++a) f.argn[a] = r.str();
        f.g.pan_x = r.i32();
        f.g.pan_y = r.i32();
        if (!r.ok) return false;
        if (!read_nodes(r, f.g.nodes)) return false;
        if (!read_links_into(r, f.g.nodes, f.g.links)) return false;
        if (f.name.empty()) f.name = "Function";
        f.name = unique_fn_name(d, f.name, -1);
        for (int a = 0; a < 4; ++a) f.argn[a] = clean_arg_name(f.argn[a]);
        d.fns.push_back(std::move(f));
        ensure_entry(d.fns.back());
    }
    return r.ok;
}

bool load_var_tail(Rdr& r, BpDoc& d) {
    if (r.p >= r.end || !r.ok) return true;
    uint32_t vc = r.u32();
    if (!r.ok || vc > 32) return true;
    for (uint32_t i = 0; i < vc; ++i) {
        BpVar v;
        v.name = r.str();
        v.type = clamp_vtype(r.i32());
        v.num = r.f32();
        v.text = r.str();
        if (!r.ok) return false;
        if (v.text.size() > 200) v.text.resize(200);
        if (v.name.empty()) v.name = "Value";
        v.name = unique_var_name(d, clean_fn_name(v.name), -1);
        if (v.name.empty()) v.name = unique_var_name(d, "Value", -1);
        d.vars.push_back(std::move(v));
    }
    return r.ok;
}

void sync_var_nodes(BpDoc& d, std::vector<BpNode>& nodes, std::vector<BpLink>& links) {
    bool changed = false;
    for (BpNode& n : nodes) {
        if (n.kind != BP_GETVAR && n.kind != BP_SETVAR) continue;
        int vi = find_var_index(d, n.name);
        if (vi < 0) continue;
        if (n.op != d.vars[vi].type) { n.op = d.vars[vi].type; changed = true; }
        n.name = d.vars[vi].name;
        /* Older Set nodes stored the number on the value itself, so every Set wrote the same thing.
           b == 0 means this node has not taken its own copy yet. */
        if (n.kind == BP_SETVAR && n.b == 0.f) {
            if (n.a == 0.f && n.text.empty()) {
                n.a = d.vars[vi].num;
                if (d.vars[vi].type == BP_STRING) n.text = d.vars[vi].text;
            }
            n.b = 1.f;
        }
    }
    if (changed) prune_bad_links(nodes, links);
}

bool load_into(BpDoc& d, const std::string& path) {
    std::string bytes = read_bin(path);
    if (bytes.size() < 16) return false;
    if (bytes.compare(0, 4, "EBP1") != 0) return false;
    Rdr r;
    r.p = bytes.data() + 4;
    r.end = bytes.data() + bytes.size();
    uint32_t ver = r.u32();
    if (!r.ok || ver != 1) return false;
    BpDoc next;
    next.path = path;
    next.pan_x = r.i32();
    next.pan_y = r.i32();
    if (!read_nodes(r, next.nodes)) return false;
    if (!read_links_into(r, next.nodes, next.links)) return false;
    if (!load_fn_tail(r, next)) next.fns.clear();
    else if (!load_var_tail(r, next)) next.vars.clear();
    sync_sig_nodes(next, next.nodes, next.links, -1);
    for (int i = 0; i < (int)next.fns.size(); ++i)
        sync_sig_nodes(next, next.fns[i].g.nodes, next.fns[i].g.links, i);
    sync_var_nodes(next, next.nodes, next.links);
    for (int i = 0; i < (int)next.fns.size(); ++i)
        sync_var_nodes(next, next.fns[i].g.nodes, next.fns[i].g.links);
    next.edit_fn = -1;
    next.event.pan_x = next.pan_x;
    next.event.pan_y = next.pan_y;
    next.event.nodes = next.nodes;
    next.event.links = next.links;
    next.open = true;
    next.dirty = false;
    d = std::move(next);
    return true;
}

void object_extra(const std::string& text, float& z, float& rot) {
    z = 0.f;
    rot = 0.f;
    if (text.empty()) return;
    std::string body = text;
    auto bar = text.find('|');
    if (bar != std::string::npos) body = text.substr(0, bar);
    std::istringstream in(body);
    in >> z >> rot;
}

float object_tween_sec(const std::string& text) {
    auto bar = text.find('|');
    if (bar == std::string::npos || bar + 1 >= text.size()) return 0.f;
    return (float)std::atof(text.c_str() + (int)bar + 1);
}

void tween_defaults(const std::string& text, float& z, float& rot, float& sec) {
    z = 0.f;
    rot = 0.f;
    sec = 0.5f;
    if (text.empty()) return;
    std::istringstream in(text);
    in >> z >> rot >> sec;
}

std::string object_ref(const BpNode& n) {
    if (n.op > 0) return "ent_find_by_id(" + std::to_string(n.op) + ")";
    if (!n.name.empty() && n.name != "(unnamed)")
        return "ent_find_by_name(" + nxa_str(n.name) + ")";
    return "-1";
}

std::string node_title_s(const BpNode& n) {
    if (n.kind == BP_OBJECT) {
        if (n.op > 0) {
            int s = engine_entity_find_by_id(n.op);
            if (s >= 0) {
                if (!g.entities[s].name.empty()) return g.entities[s].name;
                return "(unnamed)";
            }
        }
        if (!n.name.empty()) return n.name;
        return "Object";
    }
    if (n.kind == BP_MATH) {
        if (n.op == 1) return "Subtract";
        if (n.op == 2) return "Multiply";
        if (n.op == 3) return "Divide";
        return "Add";
    }
    if (n.kind == BP_KEY) return "Key " + key_label_s(n.op);
    if (n.kind == BP_ENTRY) return n.name.empty() ? "Function" : n.name;
    if (n.kind == BP_RETURN) return "Return";
    if (n.kind == BP_CALL) return n.name.empty() ? "Call" : n.name;
    if (n.kind == BP_GETVAR) return n.name.empty() ? "Value" : n.name;
    if (n.kind == BP_SETVAR) return n.name.empty() ? "Set Value" : "Set " + n.name;
    if (n.kind == BP_IF) {
        if (n.op == 1) return "If not equal";
        if (n.op == 2) return "If greater";
        if (n.op == 3) return "If less";
        if (n.op == 4) return "If at least";
        if (n.op == 5) return "If at most";
        return "If equal";
    }
    return kind_name_of(n.kind);
}

void indent_nxa(std::ostringstream& o, int n) {
    for (int i = 0; i < n; ++i) o << "    ";
}

std::string expr_out(const BpDoc& d, int node, int pin, int depth);

std::string expr_in_raw(const BpDoc& d, int node, int pin, int depth) {
    if (depth > 32 || node < 0 || node >= (int)d.nodes.size()) return "0";
    for (const BpLink& L : d.links) {
        if (L.to_node == node && L.to_pin == pin)
            return expr_out(d, L.from_node, L.from_pin, depth + 1);
    }
    const BpNode& n = d.nodes[node];
    if (n.kind == BP_IF) {
        if (pin == 2) return nxa_float(n.b);
        if (pin == 1) return nxa_float(n.a);
    }
    if (n.kind == BP_SETVAR && pin == 1) {
        int vi = find_var_index(d, n.name);
        int ty = clamp_vtype(n.op);
        if (vi >= 0) ty = d.vars[vi].type;
        if (ty == BP_BOOL) return n.a != 0.f ? "true" : "false";
        if (ty == BP_STRING) return nxa_str(n.text);
        return nxa_float(n.a);
    }
    PinInfo p = pin_info(n, pin);
    if (!p.ok) return "0";
    if (p.type == BP_BOOL) {
        if (n.kind == BP_VISIBLE && pin == 1) return n.op != 0 ? "true" : "false";
        return "false";
    }
    if (p.type == BP_STRING) return nxa_str(n.text);
    if (p.type == BP_ENTITY) return "me";
    if (p.type == BP_FLOAT) {
        if (n.kind == BP_CALL) {
            if (pin == 2) return nxa_float(n.b);
            if (pin == 3 || pin == 4) {
                float c = 0.f, dv = 0.f;
                object_extra(n.text, c, dv);
                return nxa_float(pin == 3 ? c : dv);
            }
            return nxa_float(n.a);
        }
        if (n.kind == BP_OBJECT && (pin == 3 || pin == 4)) {
            float z = 0.f, rot = 0.f;
            object_extra(n.text, z, rot);
            return nxa_float(pin == 3 ? z : rot);
        }
        if ((n.kind == BP_MATH && pin == 1) || (n.kind == BP_SET && pin == 2) || (n.kind == BP_SPAWN && pin == 2) || (n.kind == BP_OBJECT && pin == 2)
            || ((n.kind == BP_FORCE || n.kind == BP_IMPULSE || n.kind == BP_SIZE || n.kind == BP_FADE) && pin == 2)
            || (n.kind == BP_COLOR && pin == 2) || (n.kind == BP_TWEEN && pin == 2))
            return nxa_float(n.b);
        if (n.kind == BP_VEL) return nxa_float(pin == 2 ? n.b : n.a);
        if (n.kind == BP_YAW) return nxa_float(n.a);
        if (n.kind == BP_VIEW) {
            if (pin == 2) return nxa_float(n.a);
            if (pin == 3) return nxa_float(n.b);
            if (pin == 4) return nxa_float((float)n.op);
            return "0.0";
        }
        if (n.kind == BP_SPAWN3) {
            if (pin == 2) return nxa_float(n.b);
            if (pin == 1) return nxa_float(n.a);
            return "0.0";
        }
        if (n.kind == BP_COLOR && pin == 3) return nxa_float((float)n.op);
        if (n.kind == BP_TWEEN && (pin == 3 || pin == 4 || pin == 5)) {
            float z = 0.f, rot = 0.f, sec = 0.5f;
            tween_defaults(n.text, z, rot, sec);
            if (pin == 3) return nxa_float(z);
            if (pin == 4) return nxa_float(rot);
            return nxa_float(sec);
        }
        return nxa_float(n.a);
    }
    return "0";
}

std::string expr_out(const BpDoc& d, int node, int pin, int depth) {
    if (depth > 32 || node < 0 || node >= (int)d.nodes.size()) return "0";
    const BpNode& n = d.nodes[node];
    if (n.kind == BP_GETVAR) {
        int vi = find_var_index(d, n.name);
        int ty = clamp_vtype(n.op);
        if (vi >= 0) ty = d.vars[vi].type;
        if (vi < 0) {
            if (ty == BP_BOOL) return "false";
            if (ty == BP_STRING) return "\"\"";
            return "0.0";
        }
        std::string slot = std::to_string(vi);
        if (ty == BP_BOOL)
            return "bp_read_b(" + slot + ", " + (d.vars[vi].num != 0.f ? "1" : "0") + ") != 0";
        if (ty == BP_STRING)
            return "bp_read_s(" + slot + ", " + nxa_str(d.vars[vi].text) + ")";
        return "bp_read_f(" + slot + ", " + nxa_float(d.vars[vi].num) + ")";
    }
    if (n.kind == BP_ENTRY && pin >= 1 && pin <= 4)
        return "bp_a" + std::to_string(pin - 1);
    if (n.kind == BP_CALL && pin == clamp_argc(n.op) + 2)
        return "bp_c" + std::to_string(node);
    if (n.kind == BP_GET) {
        if (pin == 1) return "ent_y(me)";
        if (pin == 2) return "ent_z(me)";
        return "ent_x(me)";
    }
    if (n.kind == BP_OBJECT) {
        std::string who = object_ref(n);
        if (pin == 10) return who;
        if (pin == 6) return "ent_x(" + who + ")";
        if (pin == 7) return "ent_y(" + who + ")";
        if (pin == 8) return "ent_z(" + who + ")";
        if (pin == 9) return "ent_rot(" + who + ")";
        if (pin == 11) return "bp_yaw(" + who + ")";
    }
    if (n.kind == BP_GETPITCH) return "bp_pitch(me)";
    if (n.kind == BP_AXIS) {
        int pos = n.op;
        int neg = (int)n.a;
        if (pos < 1 || pos > 254) pos = 87;
        if (neg < 1 || neg > 254) neg = 83;
        return "bp_axis(" + std::to_string(pos) + ", " + std::to_string(neg) + ")";
    }
    if (n.kind == BP_LOOK) {
        if (pin == 3) return "bp_look_dy(bp_my" + std::to_string(node) + ")";
        return "bp_look_dx(bp_mx" + std::to_string(node) + ")";
    }
    if (n.kind == BP_VIEW && pin >= 5 && pin <= 7) {
        std::string yaw = expr_in_raw(d, node, 0, depth + 1);
        std::string pit = expr_in_raw(d, node, 1, depth + 1);
        std::string fwd = expr_in_raw(d, node, 2, depth + 1);
        std::string right = expr_in_raw(d, node, 3, depth + 1);
        std::string down = expr_in_raw(d, node, 4, depth + 1);
        const char* fn = pin == 6 ? "bp_view_y" : pin == 7 ? "bp_view_z" : "bp_view_x";
        if (pin == 6)
            return std::string("bp_view_y(") + pit + ", " + fwd + ", " + down + ")";
        return std::string(fn) + "(" + yaw + ", " + pit + ", " + fwd + ", " + right + ")";
    }
    if (n.kind == BP_WALK && pin == 6) return "bp_g" + std::to_string(node) + " != 0";
    if (n.kind == BP_KEY) {
        int vk = n.op;
        if (vk < 1 || vk > 254) vk = 32;
        return "key_down(" + std::to_string(vk) + ")";
    }
    if (n.kind == BP_MOUSE) {
        int b = n.op;
        if (b < 0 || b > 2) b = 0;
        return "eng_mouse_down(" + std::to_string(b) + ")";
    }
    if (n.kind == BP_GETVEL) return pin == 1 ? "ent_velocity_y(me)" : "ent_velocity_x(me)";
    if (n.kind == BP_GETYAW) return "bp_yaw(me)";
    if (n.kind == BP_DT) return "eng_dt_seconds()";
    if (n.kind == BP_HIT && pin == 3) return "bp_h" + std::to_string(node);
    if (n.kind == BP_SPAWN3 && pin == 6) return "bp_e" + std::to_string(node);
    if (n.kind == BP_MATH && pin == 2) {
        const char* op = "+";
        if (n.op == 1) op = "-";
        else if (n.op == 2) op = "*";
        else if (n.op == 3) op = "/";
        return "(" + expr_in_raw(d, node, 0, depth + 1) + " " + op + " " + expr_in_raw(d, node, 1, depth + 1) + ")";
    }
    if (n.kind == BP_SPAWN && pin == 4) return "bp_e" + std::to_string(node);
    return "0";
}

struct Emit {
    const BpDoc* d = nullptr;
    std::ostringstream* o = nullptr;
    std::vector<int> stack;
    std::vector<char> seen;
};

bool exec_in_linked(const BpDoc& d, int node) {
    if (node < 0 || node >= (int)d.nodes.size()) return false;
    for (const BpLink& L : d.links) {
        if (L.to_node != node) continue;
        PinInfo p = pin_info(d.nodes[node], L.to_pin);
        if (p.ok && p.dir == 0 && p.type == BP_EXEC) return true;
    }
    return false;
}

bool runs_on_its_own(int kind) {
    return kind == BP_BRANCH || kind == BP_IF || kind == BP_SEQUENCE || kind == BP_LOG
        || kind == BP_SET || kind == BP_SPAWN || kind == BP_DESTROY
        || kind == BP_VEL || kind == BP_HIT || kind == BP_SPAWN3 || kind == BP_YAW
        || kind == BP_LOOK || kind == BP_PITCH || kind == BP_WALK || kind == BP_NUDGE;
}

bool stack_has(const std::vector<int>& s, int n) {
    for (int v : s) if (v == n) return true;
    return false;
}

void emit_from(Emit& e, int node, int out_pin, int ind);

void emit_node(Emit& e, int node, int ind) {
    if (!e.d || node < 0 || node >= (int)e.d->nodes.size()) return;
    if (stack_has(e.stack, node) || (int)e.stack.size() > 64) {
        indent_nxa(*e.o, ind);
        *e.o << "// cycle skipped\n";
        return;
    }
    if ((int)e.seen.size() != (int)e.d->nodes.size()) e.seen.assign(e.d->nodes.size(), 0);
    e.seen[node] = 1;
    e.stack.push_back(node);
    const BpNode& n = e.d->nodes[node];
    std::ostringstream& o = *e.o;
    if (n.kind == BP_BRANCH) {
        indent_nxa(o, ind);
        o << "if (" << expr_in_raw(*e.d, node, 1, 0) << ") {\n";
        emit_from(e, node, 2, ind + 1);
        indent_nxa(o, ind);
        o << "} else {\n";
        emit_from(e, node, 3, ind + 1);
        indent_nxa(o, ind);
        o << "}\n";
    } else if (n.kind == BP_IF) {
        const char* op = "==";
        int c = n.op;
        if (c == 1) op = "!=";
        else if (c == 2) op = ">";
        else if (c == 3) op = "<";
        else if (c == 4) op = ">=";
        else if (c == 5) op = "<=";
        indent_nxa(o, ind);
        o << "if ((" << expr_in_raw(*e.d, node, 1, 0) << ") " << op << " ("
          << expr_in_raw(*e.d, node, 2, 0) << ")) {\n";
        emit_from(e, node, 3, ind + 1);
        indent_nxa(o, ind);
        o << "} else {\n";
        emit_from(e, node, 4, ind + 1);
        indent_nxa(o, ind);
        o << "}\n";
    } else if (n.kind == BP_SEQUENCE) {
        emit_from(e, node, 1, ind);
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_LOG) {
        indent_nxa(o, ind);
        o << "eng_log(" << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_SET) {
        indent_nxa(o, ind);
        o << "if (me >= 0) {\n";
        indent_nxa(o, ind + 1);
        o << "ent_set_x(me, (int)(" << expr_in_raw(*e.d, node, 1, 0) << "));\n";
        indent_nxa(o, ind + 1);
        o << "ent_set_y(me, (int)(" << expr_in_raw(*e.d, node, 2, 0) << "));\n";
        indent_nxa(o, ind);
        o << "}\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_SPAWN) {
        std::string kind = n.text.empty() ? "rect" : n.text;
        std::string name = n.name.empty() ? "spawned" : n.name;
        indent_nxa(o, ind);
        o << "bp_e" << node << " = ent_create(" << nxa_str(kind) << ", " << nxa_str(name)
          << ", (int)(" << expr_in_raw(*e.d, node, 1, 0) << "), (int)("
          << expr_in_raw(*e.d, node, 2, 0) << "), 48, 48, 220, 180, 70);\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_DESTROY) {
        indent_nxa(o, ind);
        o << "let bp_d" << node << ": int = " << expr_in_raw(*e.d, node, 1, 0) << ";\n";
        indent_nxa(o, ind);
        o << "if (bp_d" << node << " >= 0) ent_destroy(bp_d" << node << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_OBJECT) {
        indent_nxa(o, ind);
        o << "let bp_o" << node << ": int = " << object_ref(n) << ";\n";
        indent_nxa(o, ind);
        o << "if (bp_o" << node << " >= 0) {\n";
        auto linked = [&](int pin) {
            for (const BpLink& L : e.d->links)
                if (L.to_node == node && L.to_pin == pin) return true;
            return false;
        };
        float tween = object_tween_sec(n.text);
        std::string xs = expr_in_raw(*e.d, node, 1, 0);
        std::string ys = expr_in_raw(*e.d, node, 2, 0);
        std::string zs = expr_in_raw(*e.d, node, 3, 0);
        std::string rs = expr_in_raw(*e.d, node, 4, 0);
        if (tween > 0.001f) {
            indent_nxa(o, ind + 1);
            o << "eng_tween_pose(bp_o" << node << ", " << xs << ", " << ys << ", "
              << zs << ", " << rs << ", " << nxa_float(tween) << ");\n";
        } else {
            indent_nxa(o, ind + 1);
            o << "ent_set_x(bp_o" << node << ", (int)(" << xs << "));\n";
            indent_nxa(o, ind + 1);
            o << "ent_set_y(bp_o" << node << ", (int)(" << ys << "));\n";
            indent_nxa(o, ind + 1);
            o << "ent_set_z(bp_o" << node << ", (int)(" << zs << "));\n";
            indent_nxa(o, ind + 1);
            o << "ent_set_rot(bp_o" << node << ", (int)(" << rs << "));\n";
        }
        if (linked(12)) {
            indent_nxa(o, ind + 1);
            o << "bp_set_yaw(bp_o" << node << ", " << expr_in_raw(*e.d, node, 12, 0) << ");\n";
        }
        if (linked(13)) {
            indent_nxa(o, ind + 1);
            o << "bp_set_pitch(bp_o" << node << ", " << expr_in_raw(*e.d, node, 13, 0) << ");\n";
        }
        indent_nxa(o, ind);
        o << "}\n";
        emit_from(e, node, 5, ind);
    } else if (n.kind == BP_VEL) {
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_set_velocity(me, " << expr_in_raw(*e.d, node, 1, 0)
          << ", " << expr_in_raw(*e.d, node, 2, 0) << ");\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_YAW) {
        indent_nxa(o, ind);
        o << "if (me >= 0) bp_set_yaw(me, " << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_LOOK) {
        indent_nxa(o, ind);
        o << "bp_lock();\n";
        indent_nxa(o, ind);
        o << "bp_mx" << node << " = bp_mdx();\n";
        indent_nxa(o, ind);
        o << "bp_my" << node << " = bp_mdy();\n";
        emit_from(e, node, 1, ind);
    } else if (n.kind == BP_PITCH) {
        indent_nxa(o, ind);
        o << "if (me >= 0) bp_set_pitch(me, " << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_WALK) {
        float speed = n.a > 1.f ? n.a : 500.f;
        float jump = n.b > 1.f ? n.b : 900.f;
        indent_nxa(o, ind);
        o << "let bp_wj" << node << ": int = 0;\n";
        indent_nxa(o, ind);
        o << "if (" << expr_in_raw(*e.d, node, 4, 0) << ") { bp_wj" << node << " = 1; }\n";
        indent_nxa(o, ind);
        o << "bp_g" << node << " = bp_walk(me, " << expr_in_raw(*e.d, node, 1, 0)
          << ", " << expr_in_raw(*e.d, node, 2, 0) << ", " << expr_in_raw(*e.d, node, 3, 0)
          << ", bp_wj" << node << ", " << nxa_float(speed) << ", " << nxa_float(jump) << ");\n";
        emit_from(e, node, 5, ind);
    } else if (n.kind == BP_NUDGE) {
        float speed = n.a > 1.f ? n.a : 0.f;
        indent_nxa(o, ind);
        o << "if (me >= 0) bp_nudge(me, " << expr_in_raw(*e.d, node, 1, 0) << ", "
          << expr_in_raw(*e.d, node, 2, 0) << ", " << expr_in_raw(*e.d, node, 3, 0)
          << ", " << nxa_float(speed) << ");\n";
        emit_from(e, node, 4, ind);
    } else if (n.kind == BP_SFX) {
        indent_nxa(o, ind);
        o << "sfx_play(" << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_MUSIC) {
        int loop = n.op != 0 ? 1 : 0;
        indent_nxa(o, ind);
        o << "eng_music_play(" << expr_in_raw(*e.d, node, 1, 0) << ", " << loop << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_MUSIC_STOP) {
        indent_nxa(o, ind);
        o << "eng_music_stop();\n";
        emit_from(e, node, 1, ind);
    } else if (n.kind == BP_FORCE) {
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_apply_force(me, " << expr_in_raw(*e.d, node, 1, 0)
          << ", " << expr_in_raw(*e.d, node, 2, 0) << ");\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_IMPULSE) {
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_apply_impulse(me, " << expr_in_raw(*e.d, node, 1, 0)
          << ", " << expr_in_raw(*e.d, node, 2, 0) << ");\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_VISIBLE) {
        indent_nxa(o, ind);
        o << "let bp_vis" << node << ": int = 0;\n";
        indent_nxa(o, ind);
        o << "if (" << expr_in_raw(*e.d, node, 1, 0) << ") { bp_vis" << node << " = 1; }\n";
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_set_visible(me, bp_vis" << node << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_COLOR) {
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_set_color(me, (int)(" << expr_in_raw(*e.d, node, 1, 0)
          << "), (int)(" << expr_in_raw(*e.d, node, 2, 0) << "), (int)("
          << expr_in_raw(*e.d, node, 3, 0) << "));\n";
        emit_from(e, node, 4, ind);
    } else if (n.kind == BP_SIZE) {
        indent_nxa(o, ind);
        o << "if (me >= 0) {\n";
        indent_nxa(o, ind + 1);
        o << "ent_set_w(me, (int)(" << expr_in_raw(*e.d, node, 1, 0) << "));\n";
        indent_nxa(o, ind + 1);
        o << "ent_set_h(me, (int)(" << expr_in_raw(*e.d, node, 2, 0) << "));\n";
        indent_nxa(o, ind);
        o << "}\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_SETTEXT) {
        indent_nxa(o, ind);
        o << "if (me >= 0) ent_set_text(me, " << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_SCENE) {
        indent_nxa(o, ind);
        o << "{\n";
        indent_nxa(o, ind + 1);
        o << "let bp_sp" << node << ": string = proj_root() + \"/\" + "
          << expr_in_raw(*e.d, node, 1, 0) << ";\n";
        indent_nxa(o, ind + 1);
        o << "if (eng_fs_exists(bp_sp" << node << ") == 1) eng_scene_load(bp_sp" << node << ");\n";
        indent_nxa(o, ind);
        o << "}\n";
        emit_from(e, node, 2, ind);
    } else if (n.kind == BP_TWEEN) {
        indent_nxa(o, ind);
        o << "if (me >= 0) eng_tween_pose(me, " << expr_in_raw(*e.d, node, 1, 0) << ", "
          << expr_in_raw(*e.d, node, 2, 0) << ", " << expr_in_raw(*e.d, node, 3, 0) << ", "
          << expr_in_raw(*e.d, node, 4, 0) << ", " << expr_in_raw(*e.d, node, 5, 0) << ");\n";
        emit_from(e, node, 6, ind);
    } else if (n.kind == BP_FADE) {
        indent_nxa(o, ind);
        o << "if (me >= 0) eng_tween_fade(me, " << expr_in_raw(*e.d, node, 1, 0) << ", "
          << expr_in_raw(*e.d, node, 2, 0) << ");\n";
        emit_from(e, node, 3, ind);
    } else if (n.kind == BP_HIT) {
        std::string prefix = n.text.empty() ? "" : n.text;
        indent_nxa(o, ind);
        o << "bp_h" << node << " = ent_first_overlap(me, " << nxa_str(prefix) << ");\n";
        indent_nxa(o, ind);
        o << "if (bp_h" << node << " >= 0) {\n";
        emit_from(e, node, 1, ind + 1);
        indent_nxa(o, ind);
        o << "} else {\n";
        emit_from(e, node, 2, ind + 1);
        indent_nxa(o, ind);
        o << "}\n";
    } else if (n.kind == BP_SPAWN3) {
        std::string spec = n.text.empty() ? "sphere" : n.text;
        std::string script;
        auto bar = spec.find('|');
        if (bar != std::string::npos) {
            script = spec.substr(bar + 1);
            spec = spec.substr(0, bar);
        }
        if (spec.empty()) spec = "sphere";
        std::string name = n.name.empty() ? "spawned" : n.name;
        int sz = n.op;
        if (sz < 4) sz = 32;
        if (sz > 400) sz = 400;
        indent_nxa(o, ind);
        o << "bp_e" << node << " = ent_create(" << nxa_str(spec) << ", " << nxa_str(name)
          << ", (int)(" << expr_in_raw(*e.d, node, 1, 0) << "), (int)("
          << expr_in_raw(*e.d, node, 2, 0) << "), " << sz << ", " << sz
          << ", 255, 176, 64);\n";
        indent_nxa(o, ind);
        o << "if (bp_e" << node << " >= 0) {\n";
        indent_nxa(o, ind + 1);
        o << "ent_set_z(bp_e" << node << ", (int)(" << expr_in_raw(*e.d, node, 3, 0) << "));\n";
        indent_nxa(o, ind + 1);
        o << "bp_set_yaw(bp_e" << node << ", " << expr_in_raw(*e.d, node, 4, 0) << ");\n";
        indent_nxa(o, ind + 1);
        o << "bp_set_pitch(bp_e" << node << ", " << expr_in_raw(*e.d, node, 7, 0) << ");\n";
        if (!script.empty()) {
            indent_nxa(o, ind + 1);
            o << "ent_set_script(bp_e" << node << ", " << nxa_str(script) << ");\n";
        }
        indent_nxa(o, ind);
        o << "}\n";
        emit_from(e, node, 5, ind);
    } else if (n.kind == BP_RETURN) {
        indent_nxa(o, ind);
        o << "bp_ret = " << expr_in_raw(*e.d, node, 1, 0) << ";\n";
        indent_nxa(o, ind);
        o << "return bp_ret;\n";
    } else if (n.kind == BP_CALL) {
        int argc = clamp_argc(n.op);
        int fi = find_fn_index(*e.d, n.name);
        indent_nxa(o, ind);
        if (fi < 0) {
            o << "// missing function\n";
            indent_nxa(o, ind);
            o << "bp_c" << node << " = 0.0;\n";
        } else {
            argc = e.d->fns[fi].argc;
            o << "bp_c" << node << " = " << fn_ident(*e.d, fi) << "(";
            for (int a = 0; a < argc; ++a) {
                if (a) o << ", ";
                o << expr_in_raw(*e.d, node, a + 1, 0);
            }
            o << ");\n";
        }
        emit_from(e, node, argc + 1, ind);
    } else if (n.kind == BP_SETVAR) {
        int vi = find_var_index(*e.d, n.name);
        int ty = clamp_vtype(n.op);
        indent_nxa(o, ind);
        if (vi < 0) {
            o << "// missing value\n";
        } else if (ty == BP_BOOL) {
            o << "let bp_vb" << node << ": int = 0;\n";
            indent_nxa(o, ind);
            o << "if (" << expr_in_raw(*e.d, node, 1, 0) << ") { bp_vb" << node << " = 1; }\n";
            indent_nxa(o, ind);
            o << "bp_write_b(" << vi << ", bp_vb" << node << ");\n";
        } else if (ty == BP_STRING) {
            o << "bp_write_s(" << vi << ", " << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        } else {
            o << "bp_write_f(" << vi << ", " << expr_in_raw(*e.d, node, 1, 0) << ");\n";
        }
        emit_from(e, node, 2, ind);
    }
    e.stack.pop_back();
}

void emit_from(Emit& e, int node, int out_pin, int ind) {
    if (!e.d) return;
    for (const BpLink& L : e.d->links) {
        if (L.from_node == node && L.from_pin == out_pin) {
            emit_node(e, L.to_node, ind);
            return;
        }
    }
}

void emit_locals(const BpDoc& d, std::ostringstream& o) {
    for (int i = 0; i < (int)d.nodes.size(); ++i) {
        if (d.nodes[i].kind == BP_SPAWN || d.nodes[i].kind == BP_SPAWN3)
            o << "    let bp_e" << i << ": int = -1;\n";
        if (d.nodes[i].kind == BP_HIT)
            o << "    let bp_h" << i << ": int = -1;\n";
        if (d.nodes[i].kind == BP_LOOK) {
            o << "    let bp_mx" << i << ": int = 0;\n";
            o << "    let bp_my" << i << ": int = 0;\n";
        }
        if (d.nodes[i].kind == BP_WALK)
            o << "    let bp_g" << i << ": int = 0;\n";
        if (d.nodes[i].kind == BP_CALL)
            o << "    let bp_c" << i << ": float = 0.0;\n";
    }
}

void emit_fn(const BpDoc& d, int event_kind, std::ostringstream& o, bool update, bool as_ext) {
    if (as_ext) o << (update ? "fn ext_update() {\n" : "fn ext_init() {\n");
    else o << (update ? "fn script_update() {\n" : "fn script_init() {\n");
    if (as_ext) o << "    let me: int = -1;\n";
    else o << "    let me: int = eng_self();\n";
    emit_locals(d, o);
    if (update && !as_ext) o << "    if (me < 0) return;\n";
    Emit e;
    e.d = &d;
    e.o = &o;
    e.seen.assign(d.nodes.size(), 0);
    int ev = find_kind(d, event_kind);
    if (ev >= 0) emit_from(e, ev, 0, 1);
    /* A Log or Branch with nothing wired into its exec pin still runs every frame.
       Otherwise a key wired only into If never prints. */
    if (update) {
        for (int i = 0; i < (int)d.nodes.size(); ++i) {
            if (e.seen[i]) continue;
            if (!runs_on_its_own(d.nodes[i].kind)) continue;
            if (exec_in_linked(d, i)) continue;
            emit_node(e, i, 1);
        }
    }
    o << "}\n";
}

void emit_user(const BpDoc& full, int index, std::ostringstream& o) {
    if (index < 0 || index >= (int)full.fns.size()) return;
    const BpFn& fn = full.fns[index];
    BpDoc walk = full;
    walk.nodes = fn.g.nodes;
    walk.links = fn.g.links;
    walk.edit_fn = index;
    o << "fn " << fn_ident(full, index) << "(";
    for (int a = 0; a < fn.argc; ++a) {
        if (a) o << ", ";
        o << "bp_a" << a << ": float";
    }
    o << "): float {\n";
    o << "    let me: int = eng_self();\n";
    o << "    let bp_ret: float = 0.0;\n";
    emit_locals(walk, o);
    Emit e;
    e.d = &walk;
    e.o = &o;
    e.seen.assign(walk.nodes.size(), 0);
    int entry = find_kind(walk, BP_ENTRY);
    if (entry >= 0) emit_from(e, entry, 0, 1);
    o << "    return bp_ret;\n";
    o << "}\n";
}

void emit_value_api(std::ostringstream& o) {
    o << "inline_cpp! {\n";
    o << "extern \"C\" float engine_bp_value_get(int, int, float);\n";
    o << "extern \"C\" void engine_bp_value_set(int, int, float);\n";
    o << "extern \"C\" int engine_bp_value_get_bool(int, int, int);\n";
    o << "extern \"C\" void engine_bp_value_set_bool(int, int, int);\n";
    o << "extern \"C\" const char* engine_bp_value_get_str(int, int, const char*);\n";
    o << "extern \"C\" void engine_bp_value_set_str(int, int, const char*);\n";
    o << "}\n\n";
    o << "fn bp_read_f(slot: int, fallback: float): float {\n";
    o << "    let me: int = eng_self();\n";
    o << "    let r: float = fallback;\n";
    o << "    inline_cpp! { r = engine_bp_value_get(me, slot, fallback); }\n";
    o << "    return r;\n";
    o << "}\n\n";
    o << "fn bp_write_f(slot: int, v: float) {\n";
    o << "    let me: int = eng_self();\n";
    o << "    inline_cpp! { engine_bp_value_set(me, slot, v); }\n";
    o << "}\n\n";
    o << "fn bp_read_b(slot: int, fallback: int): int {\n";
    o << "    let me: int = eng_self();\n";
    o << "    let r: int = fallback;\n";
    o << "    inline_cpp! { r = engine_bp_value_get_bool(me, slot, fallback); }\n";
    o << "    return r;\n";
    o << "}\n\n";
    o << "fn bp_write_b(slot: int, v: int) {\n";
    o << "    let me: int = eng_self();\n";
    o << "    inline_cpp! { engine_bp_value_set_bool(me, slot, v); }\n";
    o << "}\n\n";
    o << "fn bp_read_s(slot: int, fallback: string): string {\n";
    o << "    let me: int = eng_self();\n";
    o << "    let r: string = fallback;\n";
    o << "    inline_cpp! { r = engine_bp_value_get_str(me, slot, fallback.c_str()); }\n";
    o << "    return r;\n";
    o << "}\n\n";
    o << "fn bp_write_s(slot: int, v: string) {\n";
    o << "    let me: int = eng_self();\n";
    o << "    inline_cpp! { engine_bp_value_set_str(me, slot, v.c_str()); }\n";
    o << "}\n\n";
}

std::string generate_nxa(const BpDoc& src) {
    BpDoc d = src;
    flush_active(d);
    sync_sig_nodes(d, d.event.nodes, d.event.links, -1);
    for (int i = 0; i < (int)d.fns.size(); ++i)
        sync_sig_nodes(d, d.fns[i].g.nodes, d.fns[i].g.links, i);
    sync_var_nodes(d, d.event.nodes, d.event.links);
    for (int i = 0; i < (int)d.fns.size(); ++i)
        sync_var_nodes(d, d.fns[i].g.nodes, d.fns[i].g.links);
    std::ostringstream o;
    o << "// EYESCIRE_BLUEPRINT " << file_name_of(d.path) << "\n";
    o << "// Generated by Eyescire. Edit the blueprint, not this file.\n";
    o << "#include <eyescire/script>\n\n";
    o << "inline_cpp! {\n";
    o << "extern \"C\" int engine_entity_rot_y(int);\n";
    o << "extern \"C\" void engine_entity_set_rot_y(int, int);\n";
    o << "extern \"C\" int engine_entity_rot_x(int);\n";
    o << "extern \"C\" void engine_entity_set_rot_x(int, int);\n";
    o << "extern \"C\" int engine_fps_move(int, float, float, float, int, float, float);\n";
    o << "extern \"C\" void engine_mouse_lock(int);\n";
    o << "extern \"C\" void engine_mouse_lock_poll();\n";
    o << "extern \"C\" int engine_mouse_lock_dx();\n";
    o << "extern \"C\" int engine_mouse_lock_dy();\n";
    o << "extern \"C\" double engine_dt_seconds();\n";
    o << "extern \"C\" int engine_entity_x(int);\n";
    o << "extern \"C\" int engine_entity_y(int);\n";
    o << "extern \"C\" int engine_entity_z(int);\n";
    o << "extern \"C\" void engine_entity_set_x(int, int);\n";
    o << "extern \"C\" void engine_entity_set_y(int, int);\n";
    o << "extern \"C\" void engine_entity_set_z(int, int);\n";
    o << "extern \"C\" void engine_entity_set_look(int, float, float);\n";
    o << "extern \"C\" float engine_entity_look_yaw(int);\n";
    o << "extern \"C\" float engine_entity_look_pitch(int);\n";
    o << "}\n";
    /* eng_tween_pose / eng_tween_fade come from <eyescire/script> (api.nxa). */
    o << "fn bp_yaw(slot: int): float {\n";
    o << "    let r: float = 0.0;\n";
    o << "    inline_cpp! { r = engine_entity_look_yaw(slot); }\n";
    o << "    return r;\n";
    o << "}\n";
    o << "fn bp_set_yaw(slot: int, deg: float) {\n";
    o << "    let pitch: float = 0.0;\n";
    o << "    inline_cpp! { pitch = engine_entity_look_pitch(slot); }\n";
    o << "    inline_cpp! { engine_entity_set_look(slot, deg, pitch); }\n";
    o << "}\n";
    o << "fn bp_pitch(slot: int): float {\n";
    o << "    let r: float = 0.0;\n";
    o << "    inline_cpp! { r = engine_entity_look_pitch(slot); }\n";
    o << "    return r;\n";
    o << "}\n";
    o << "fn bp_set_pitch(slot: int, deg: float) {\n";
    o << "    if (deg > 85.0) { deg = 85.0; }\n";
    o << "    if (deg < -85.0) { deg = -85.0; }\n";
    o << "    let yaw: float = 0.0;\n";
    o << "    inline_cpp! { yaw = engine_entity_look_yaw(slot); }\n";
    o << "    inline_cpp! { engine_entity_set_look(slot, yaw, deg); }\n";
    o << "}\n";
    o << "fn bp_axis(pos: int, neg: int): float {\n";
    o << "    let v: float = 0.0;\n";
    o << "    if (key_down(pos)) { v = v + 1.0; }\n";
    o << "    if (key_down(neg)) { v = v - 1.0; }\n";
    o << "    return v;\n";
    o << "}\n";
    o << "fn bp_lock() {\n";
    o << "    inline_cpp! { engine_mouse_lock(1); engine_mouse_lock_poll(); }\n";
    o << "}\n";
    o << "fn bp_mdx(): int {\n";
    o << "    let r: int = 0;\n";
    o << "    inline_cpp! { r = engine_mouse_lock_dx(); }\n";
    o << "    return r;\n";
    o << "}\n";
    o << "fn bp_mdy(): int {\n";
    o << "    let r: int = 0;\n";
    o << "    inline_cpp! { r = engine_mouse_lock_dy(); }\n";
    o << "    return r;\n";
    o << "}\n";
    o << "fn bp_look_dx(px: int): float {\n";
    o << "    let f: float = px;\n";
    o << "    return (0.0 - f) * 0.12;\n";
    o << "}\n";
    o << "fn bp_look_dy(px: int): float {\n";
    o << "    let f: float = px;\n";
    o << "    return (0.0 - f) * 0.10;\n";
    o << "}\n";
    o << "fn bp_view_x(yaw: float, pitch: float, fwd: float, right: float): float {\n";
    o << "    let yr: float = yaw * 0.0174533;\n";
    o << "    let pr: float = pitch * 0.0174533;\n";
    o << "    return math.cos(pr) * math.cos(yr) * fwd + math.sin(yr) * right;\n";
    o << "}\n";
    o << "fn bp_view_y(pitch: float, fwd: float, down: float): float {\n";
    o << "    let pr: float = pitch * 0.0174533;\n";
    o << "    return (0.0 - math.sin(pr) * fwd) + down;\n";
    o << "}\n";
    o << "fn bp_view_z(yaw: float, pitch: float, fwd: float, right: float): float {\n";
    o << "    let yr: float = yaw * 0.0174533;\n";
    o << "    let pr: float = pitch * 0.0174533;\n";
    o << "    return math.cos(pr) * math.sin(yr) * fwd - math.cos(yr) * right;\n";
    o << "}\n";
    o << "fn bp_walk(slot: int, yaw: float, fwd: float, strafe: float, jump: int, speed: float, jump_speed: float): int {\n";
    o << "    let r: int = 0;\n";
    o << "    inline_cpp! { r = engine_fps_move(slot, yaw, fwd, strafe, jump, speed, jump_speed); }\n";
    o << "    return r;\n";
    o << "}\n";
    o << "fn bp_nudge(slot: int, x: float, y: float, z: float, speed: float) {\n";
    o << "    inline_cpp! {\n";
    o << "        float dt = (float)engine_dt_seconds();\n";
    o << "        if (dt < 0.001f) dt = 0.016f;\n";
    o << "        float scale = speed > 0.f ? speed * dt : 1.f;\n";
    o << "        engine_entity_set_x(slot, engine_entity_x(slot) + (int)(x * scale));\n";
    o << "        engine_entity_set_y(slot, engine_entity_y(slot) + (int)(y * scale));\n";
    o << "        engine_entity_set_z(slot, engine_entity_z(slot) + (int)(z * scale));\n";
    o << "    }\n";
    o << "}\n\n";
    if (!d.vars.empty()) emit_value_api(o);
    for (int i = 0; i < (int)d.fns.size(); ++i) {
        emit_user(d, i, o);
        o << "\n";
    }
    BpDoc ev = d;
    ev.nodes = d.event.nodes;
    ev.links = d.event.links;
    bool as_ext = path_is_extension_ebp(d.path);
    emit_fn(ev, BP_INIT, o, false, as_ext);
    o << "\n";
    emit_fn(ev, BP_UPDATE, o, true, as_ext);
    if (as_ext) {
        o << "\n";
        o << "fn ext_editor() {\n";
        o << "}\n\n";
        o << "fn ext_shutdown() {\n";
        o << "}\n";
    }
    return o.str();
}

int translate_doc(const BpDoc& d, bool force = false) {
    if (d.path.empty()) return 0;
    std::string nxa = nxa_for(d.path);
    std::string fresh = generate_nxa(d);
    std::string old = read_all(nxa);
    if (!force && !old.empty() && !text_is_generated(old)) {
        std::string msg = "[blueprint] refused to overwrite " + file_name_of(nxa) + " (not a generated script)";
        engine_log(msg.c_str());
        return 0;
    }
    if (old == fresh) return 1;
    std::ofstream o(nxa, std::ios::binary | std::ios::trunc);
    if (!o) {
        std::string msg = "[blueprint] could not write " + nxa;
        engine_log(msg.c_str());
        return 0;
    }
    o << fresh;
    if (!o) return 0;
    std::string msg = "[blueprint] wrote " + file_name_of(nxa);
    engine_log(msg.c_str());
    return 1;
}

BpNode* node_at(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.nodes.size()) return nullptr;
    return &g_bp.nodes[i];
}

void touch() { if (g_bp.open) g_bp.dirty = true; }

} // namespace

extern "C" int engine_project_script_generated(int i) {
    if (i < 0 || i >= (int)g.projScripts.size()) return 0;
    std::string want = rel_key(rel_with_ext(g.projScripts[i], ".ebp"));
    for (const std::string& b : g.projBlueprints)
        if (rel_key(b) == want) return 1;
    return 0;
}

extern "C" int engine_project_blueprint_count() { return (int)g.projBlueprints.size(); }

extern "C" const char* engine_project_blueprint_name(int i) {
    if (i < 0 || i >= (int)g.projBlueprints.size()) return "";
    return g.projBlueprints[i].c_str();
}

extern "C" const char* engine_project_blueprint_path(int i) {
    if (i < 0 || i >= (int)g.projBlueprints.size()) return "";
    g.projTmp = proj_join(proj_join(g.projRoot, "scripts"), g.projBlueprints[i]);
    return g.projTmp.c_str();
}

extern "C" const char* engine_project_blueprint_script(int i) {
    if (i < 0 || i >= (int)g.projBlueprints.size()) return "";
    g.projTmp = rel_with_ext(g.projBlueprints[i], ".nxa");
    return g.projTmp.c_str();
}

extern "C" int engine_bp_create_extension(const char* folder) {
    if (!folder || !folder[0]) return 0;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        fs::create_directories(folder, ec);
        if (ec) return 0;
    }
    std::string ebp = (fs::path(folder) / "ext.ebp").string();
    if (fs::exists(ebp, ec)) {
        engine_log("[blueprint] extension already has ext.ebp");
        return 0;
    }
    BpDoc d;
    d.open = true;
    d.path = ebp;
    default_nodes(d);
    d.dirty = true;
    if (!save_doc(d)) {
        engine_log("[blueprint] could not write extension ext.ebp");
        return 0;
    }
    if (!translate_doc(d, true)) {
        engine_log("[blueprint] could not write extension ext.nxa");
        return 0;
    }
    g_bp = std::move(d);
    engine_log("[blueprint] created extension ext.ebp");
    return 1;
}

extern "C" int engine_bp_translate_file(const char* ebp_path, int force) {
    if (!ebp_path || !ebp_path[0]) return 0;
    if (g_bp.open && same_path(g_bp.path, ebp_path)) {
        if (g_bp.dirty) {
            if (!save_doc(g_bp)) return 0;
        }
        return translate_doc(g_bp, force != 0) ? 1 : 0;
    }
    BpDoc d;
    if (!load_into(d, ebp_path)) {
        std::string msg = std::string("[blueprint] could not read ") + ebp_path;
        engine_log(msg.c_str());
        return 0;
    }
    return translate_doc(d, force != 0) ? 1 : 0;
}

extern "C" int engine_bp_create(const char* name) {
    if (!g.projLoaded) {
        engine_log("[blueprint] open a project first");
        return 0;
    }
    std::string base = sanitize_base(name);
    if (base.empty()) {
        engine_log("[blueprint] name needs a letter or number");
        return 0;
    }
    std::string dir = proj_join(g.projRoot, "scripts");
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string ebp = proj_join(dir, base + ".ebp");
    std::string nxa = proj_join(dir, base + ".nxa");
    if (fs::exists(ebp, ec)) {
        std::string msg = "[blueprint] already exists: " + base + ".ebp";
        engine_log(msg.c_str());
        return 0;
    }
    if (fs::exists(nxa, ec)) {
        std::string old = read_all(nxa);
        if (!old.empty() && !text_is_generated(old)) {
            std::string msg = "[blueprint] " + base + ".nxa is a hand-written script. Pick another name.";
            engine_log(msg.c_str());
            return 0;
        }
    }
    BpDoc d;
    d.open = true;
    d.path = ebp;
    default_nodes(d);
    d.dirty = true;
    if (!save_doc(d)) {
        engine_log("[blueprint] could not write the blueprint file");
        return 0;
    }
    g_bp = std::move(d);
    proj_scan_dir(proj_join(g.projRoot, "scripts"), ".ebp", g.projBlueprints);
    std::string msg = "[blueprint] created " + base + ".ebp";
    engine_log(msg.c_str());
    return 1;
}

extern "C" int engine_bp_open(const char* path) {
    if (!path || !path[0]) return 0;
    BpDoc d;
    if (!load_into(d, path)) {
        std::string msg = std::string("[blueprint] could not read ") + path;
        engine_log(msg.c_str());
        return 0;
    }
    g_bp = std::move(d);
    return 1;
}

extern "C" void engine_bp_close() { g_bp = BpDoc{}; }

extern "C" int engine_bp_is_open() { return g_bp.open ? 1 : 0; }
extern "C" const char* engine_bp_path() { return g_bp.path.c_str(); }
extern "C" int engine_bp_dirty() { return (g_bp.open && g_bp.dirty) ? 1 : 0; }

extern "C" int engine_bp_save() {
    if (!g_bp.open) return 0;
    if (!save_doc(g_bp)) return 0;
    return 1;
}

extern "C" int engine_bp_prepare_build() {
    if (g_bp.open && g_bp.dirty) {
        if (!save_doc(g_bp)) engine_log("[blueprint] save failed before build");
    }
    if (!g.projLoaded) {
        if (g_bp.open && !g_bp.path.empty()) return translate_doc(g_bp) ? 0 : 1;
        return 0;
    }
    std::string dir = proj_join(g.projRoot, "scripts");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int fails = 0;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code rec;
        if (it->is_regular_file(rec) && !rec) {
            std::string name = it->path().filename().string();
            if (ends_with_i(name, ".ebp")) {
                std::string full = it->path().string();
                if (g_bp.open && same_path(g_bp.path, full)) {
                    if (!translate_doc(g_bp)) ++fails;
                } else {
                    BpDoc tmp;
                    if (!load_into(tmp, full) || !translate_doc(tmp)) ++fails;
                }
            }
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    proj_scan_dir(dir, ".ebp", g.projBlueprints);
    return fails;
}

extern "C" int engine_bp_pan_x() { return g_bp.pan_x; }
extern "C" int engine_bp_pan_y() { return g_bp.pan_y; }
extern "C" void engine_bp_set_pan(int x, int y) {
    if (!g_bp.open) return;
    if (g_bp.pan_x == x && g_bp.pan_y == y) return;
    g_bp.pan_x = x;
    g_bp.pan_y = y;
}

extern "C" int engine_bp_node_count() { return g_bp.open ? (int)g_bp.nodes.size() : 0; }

extern "C" int engine_bp_node_kind(int i) {
    BpNode* n = node_at(i);
    return n ? n->kind : 0;
}

extern "C" const char* engine_bp_node_title(int i) {
    BpNode* n = node_at(i);
    g_bpRet = n ? node_title_s(*n) : "";
    return g_bpRet.c_str();
}

extern "C" int engine_bp_node_x(int i) {
    BpNode* n = node_at(i);
    return n ? n->x : 0;
}
extern "C" int engine_bp_node_y(int i) {
    BpNode* n = node_at(i);
    return n ? n->y : 0;
}
extern "C" void engine_bp_node_set_pos(int i, int x, int y) {
    BpNode* n = node_at(i);
    if (!n) return;
    if (n->x == x && n->y == y) return;
    n->x = x;
    n->y = y;
    touch();
}
extern "C" int engine_bp_node_op(int i) {
    BpNode* n = node_at(i);
    return n ? n->op : 0;
}
extern "C" void engine_bp_node_set_op(int i, int op) {
    BpNode* n = node_at(i);
    if (!n) return;
    if (n->kind == BP_ENTRY || n->kind == BP_CALL || n->kind == BP_GETVAR || n->kind == BP_SETVAR) return;
    if (n->kind == BP_MATH) {
        if (op < 0) op = 0;
        if (op > 3) op = 3;
    }
    if (n->kind == BP_IF) {
        if (op < 0) op = 0;
        if (op > 5) op = 5;
    }
    if (n->op == op) return;
    n->op = op;
    touch();
}
extern "C" const char* engine_bp_node_text(int i) {
    BpNode* n = node_at(i);
    return n ? n->text.c_str() : "";
}
extern "C" void engine_bp_node_set_text(int i, const char* s) {
    BpNode* n = node_at(i);
    if (!n) return;
    std::string v = s ? s : "";
    if (n->text == v) return;
    n->text = v;
    touch();
}
extern "C" const char* engine_bp_node_name(int i) {
    BpNode* n = node_at(i);
    return n ? n->name.c_str() : "";
}
extern "C" void engine_bp_node_set_name(int i, const char* s) {
    BpNode* n = node_at(i);
    if (!n) return;
    std::string v = s ? s : "";
    if (n->name == v) return;
    n->name = v;
    touch();
}
extern "C" const char* engine_bp_a_text(int i) {
    BpNode* n = node_at(i);
    g_bpRet = n ? nxa_float(n->a) : "0.0";
    return g_bpRet.c_str();
}
extern "C" const char* engine_bp_b_text(int i) {
    BpNode* n = node_at(i);
    g_bpRet = n ? nxa_float(n->b) : "0.0";
    return g_bpRet.c_str();
}
extern "C" int engine_bp_set_a_text(int i, const char* s) {
    BpNode* n = node_at(i);
    if (!n) return 0;
    float v = 0.f;
    if (!parse_f(s, &v)) return 0;
    if (n->a == v) return 1;
    n->a = v;
    touch();
    return 1;
}
extern "C" int engine_bp_set_b_text(int i, const char* s) {
    BpNode* n = node_at(i);
    if (!n) return 0;
    float v = 0.f;
    if (!parse_f(s, &v)) return 0;
    if (n->b == v) return 1;
    n->b = v;
    touch();
    return 1;
}
extern "C" void engine_bp_node_set_ab(int i, float a, float b) {
    BpNode* n = node_at(i);
    if (!n) return;
    if (n->a == a && n->b == b) return;
    n->a = a;
    n->b = b;
    touch();
}

extern "C" const char* engine_bp_key_label(int op) {
    g_bpRet = key_label_s(op);
    return g_bpRet.c_str();
}
extern "C" int engine_bp_set_key_label(int i, const char* s) {
    BpNode* n = node_at(i);
    if (!n) return 0;
    int vk = parse_key(s);
    if (vk < 0) return 0;
    if (n->op == vk) return 1;
    n->op = vk;
    touch();
    return 1;
}

extern "C" int engine_bp_add_node(int kind, int x, int y) {
    if (!g_bp.open) return -1;
    if (kind < BP_INIT || kind > BP_FADE) return -1;
    if (kind == BP_ENTRY) return -1;
    if (kind == BP_RETURN && g_bp.edit_fn < 0) return -1;
    if ((kind == BP_INIT || kind == BP_UPDATE) && g_bp.edit_fn >= 0) return -1;
    if ((kind == BP_INIT || kind == BP_UPDATE) && find_kind(g_bp, kind) >= 0) return -1;
    if (g_bp.nodes.size() >= 512) return -1;
    BpNode n;
    n.kind = kind;
    n.x = x;
    n.y = y;
    if (kind == BP_LOG) n.text = "hello";
    if (kind == BP_KEY) n.op = 32;
    if (kind == BP_SPAWN) { n.text = "rect"; n.name = "spawned"; }
    if (kind == BP_HIT) n.text = "crate";
    if (kind == BP_SPAWN3) { n.text = "sphere"; n.name = "spawned"; n.op = 32; }
    if (kind == BP_MOUSE) n.op = 0;
    if (kind == BP_SFX) n.text = "sfx/click.wav";
    if (kind == BP_MUSIC) { n.text = "music.ogg"; n.op = 1; }
    if (kind == BP_VISIBLE) n.op = 1;
    if (kind == BP_COLOR) { n.a = 255.f; n.b = 255.f; n.op = 255; }
    if (kind == BP_SIZE) { n.a = 48.f; n.b = 48.f; }
    if (kind == BP_SETTEXT) n.text = "Hello";
    if (kind == BP_SCENE) n.text = "scenes/main.scene";
    if (kind == BP_TWEEN) { n.a = 0.f; n.b = 0.f; n.text = "0 0 0.5"; }
    if (kind == BP_FADE) { n.a = 0.f; n.b = 0.5f; }
    g_bp.nodes.push_back(n);
    touch();
    return (int)g_bp.nodes.size() - 1;
}

extern "C" void engine_bp_remove_node(int i) {
    BpNode* n = node_at(i);
    if (!n) return;
    if (n->kind == BP_INIT || n->kind == BP_UPDATE || n->kind == BP_ENTRY) return;
    erase_node_at(g_bp.nodes, g_bp.links, i);
    touch();
}

extern "C" int engine_bp_link_count() { return g_bp.open ? (int)g_bp.links.size() : 0; }
extern "C" int engine_bp_link_from_node(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.links.size()) return -1;
    return g_bp.links[i].from_node;
}
extern "C" int engine_bp_link_from_pin(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.links.size()) return -1;
    return g_bp.links[i].from_pin;
}
extern "C" int engine_bp_link_to_node(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.links.size()) return -1;
    return g_bp.links[i].to_node;
}
extern "C" int engine_bp_link_to_pin(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.links.size()) return -1;
    return g_bp.links[i].to_pin;
}

extern "C" int engine_bp_add_link(int fn, int fp, int tn, int tp) {
    if (!g_bp.open) return 0;
    if (!link_types_ok(g_bp, fn, fp, tn, tp)) return 0;
    orient_link(g_bp, fn, fp, tn, tp);
    PinInfo src = pin_info(g_bp.nodes[fn], fp);
    g_bp.links.erase(std::remove_if(g_bp.links.begin(), g_bp.links.end(), [&](const BpLink& L) {
        if (L.to_node == tn && L.to_pin == tp) return true;
        if (src.ok && src.type == BP_EXEC && L.from_node == fn && L.from_pin == fp) return true;
        return false;
    }), g_bp.links.end());
    BpLink L;
    L.from_node = fn;
    L.from_pin = fp;
    L.to_node = tn;
    L.to_pin = tp;
    g_bp.links.push_back(L);
    touch();
    return 1;
}

extern "C" void engine_bp_clear_pin(int node, int pin) {
    if (!node_at(node)) return;
    size_t before = g_bp.links.size();
    g_bp.links.erase(std::remove_if(g_bp.links.begin(), g_bp.links.end(), [&](const BpLink& L) {
        return (L.from_node == node && L.from_pin == pin) || (L.to_node == node && L.to_pin == pin);
    }), g_bp.links.end());
    if (g_bp.links.size() != before) touch();
}

extern "C" int engine_bp_pin_count(int kind) { return pins_for(kind).count; }
extern "C" int engine_bp_pin_dir(int kind, int pin) {
    const PinDef* p = pin_def(kind, pin);
    return p ? p->dir : 0;
}
extern "C" int engine_bp_pin_type(int kind, int pin) {
    const PinDef* p = pin_def(kind, pin);
    return p ? p->type : 0;
}
extern "C" const char* engine_bp_pin_name(int kind, int pin) {
    const PinDef* p = pin_def(kind, pin);
    return p && p->name ? p->name : "";
}
extern "C" const char* engine_bp_kind_name(int kind) { return kind_name_of(kind); }

extern "C" int engine_bp_node_pin_count(int node) {
    BpNode* n = node_at(node);
    return n ? pin_count_of(*n) : 0;
}
extern "C" int engine_bp_node_pin_dir(int node, int pin) {
    BpNode* n = node_at(node);
    if (!n) return 0;
    PinInfo p = pin_info(*n, pin);
    return p.ok ? p.dir : 0;
}
extern "C" int engine_bp_node_pin_type(int node, int pin) {
    BpNode* n = node_at(node);
    if (!n) return 0;
    PinInfo p = pin_info(*n, pin);
    return p.ok ? p.type : 0;
}
extern "C" const char* engine_bp_node_pin_name(int node, int pin) {
    BpNode* n = node_at(node);
    if (!n) return "";
    PinInfo p = pin_info(*n, pin);
    if (!p.ok) return "";
    int argc = clamp_argc(n->op);
    if ((n->kind == BP_ENTRY || n->kind == BP_CALL) && pin >= 1 && pin <= argc) {
        g_bpRet = arg_name_of(g_bp, *n, pin - 1);
        return g_bpRet.c_str();
    }
    return p.name ? p.name : "";
}

extern "C" int engine_bp_fn_count() { return g_bp.open ? (int)g_bp.fns.size() : 0; }

extern "C" const char* engine_bp_fn_name(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.fns.size()) return "";
    return g_bp.fns[i].name.c_str();
}

extern "C" int engine_bp_fn_find(const char* name) {
    if (!g_bp.open || !name) return -1;
    return find_fn_index(g_bp, name);
}

extern "C" int engine_bp_editing() {
    if (!g_bp.open) return -1;
    return g_bp.edit_fn;
}

extern "C" void engine_bp_edit_fn(int i) {
    if (!g_bp.open) return;
    if (i < -1) i = -1;
    if (i >= (int)g_bp.fns.size()) return;
    if (i == g_bp.edit_fn) return;
    show_edit(g_bp, i);
}

extern "C" int engine_bp_add_fn() {
    if (!g_bp.open) return -1;
    if (g_bp.fns.size() >= 32) return -1;
    flush_active(g_bp);
    BpFn f;
    f.name = unique_fn_name(g_bp, "Function", -1);
    f.argc = 1;
    f.argn[0] = "a";
    f.g.pan_x = 40;
    f.g.pan_y = 40;
    BpNode entry;
    entry.kind = BP_ENTRY;
    entry.x = 40;
    entry.y = 40;
    entry.op = 1;
    entry.name = f.name;
    f.g.nodes.push_back(entry);
    g_bp.fns.push_back(std::move(f));
    int idx = (int)g_bp.fns.size() - 1;
    g_bp.edit_fn = idx;
    apply_graph(g_bp, g_bp.fns[idx].g);
    touch();
    return idx;
}

extern "C" void engine_bp_remove_fn(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.fns.size()) return;
    flush_active(g_bp);
    std::string name = g_bp.fns[i].name;
    strip_calls(g_bp.event.nodes, g_bp.event.links, name);
    for (int j = 0; j < (int)g_bp.fns.size(); ++j) {
        if (j == i) continue;
        strip_calls(g_bp.fns[j].g.nodes, g_bp.fns[j].g.links, name);
    }
    int cur = g_bp.edit_fn;
    g_bp.fns.erase(g_bp.fns.begin() + i);
    if (cur == i) g_bp.edit_fn = -1;
    else if (cur > i) g_bp.edit_fn = cur - 1;
    reload_active(g_bp);
    touch();
}

extern "C" int engine_bp_fn_argc() {
    if (!g_bp.open || g_bp.edit_fn < 0 || g_bp.edit_fn >= (int)g_bp.fns.size()) return 0;
    return g_bp.fns[g_bp.edit_fn].argc;
}

extern "C" void engine_bp_fn_set_argc(int n) {
    if (!g_bp.open || g_bp.edit_fn < 0 || g_bp.edit_fn >= (int)g_bp.fns.size()) return;
    n = clamp_argc(n);
    int idx = g_bp.edit_fn;
    if (g_bp.fns[idx].argc == n) return;
    flush_active(g_bp);
    int old = g_bp.fns[idx].argc;
    std::string name = g_bp.fns[idx].name;
    g_bp.fns[idx].argc = n;
    auto apply_one = [&](std::vector<BpNode>& nodes, std::vector<BpLink>& links, bool entryHere) {
        for (int i = 0; i < (int)nodes.size(); ++i) {
            BpNode& node = nodes[i];
            if (entryHere && node.kind == BP_ENTRY) {
                remap_node_links(links, i, BP_ENTRY, old, n);
                node.op = n;
            } else if (node.kind == BP_CALL && eq_i(node.name, name)) {
                remap_node_links(links, i, BP_CALL, old, n);
                node.op = n;
            }
        }
        prune_bad_links(nodes, links);
    };
    apply_one(g_bp.event.nodes, g_bp.event.links, false);
    for (int j = 0; j < (int)g_bp.fns.size(); ++j)
        apply_one(g_bp.fns[j].g.nodes, g_bp.fns[j].g.links, j == idx);
    reload_active(g_bp);
    touch();
}

extern "C" const char* engine_bp_fn_arg(int slot) {
    if (!g_bp.open || g_bp.edit_fn < 0 || g_bp.edit_fn >= (int)g_bp.fns.size()) return "";
    if (slot < 0 || slot > 3) return "";
    return g_bp.fns[g_bp.edit_fn].argn[slot].c_str();
}

extern "C" void engine_bp_fn_set_arg(int slot, const char* s) {
    if (!g_bp.open || g_bp.edit_fn < 0 || g_bp.edit_fn >= (int)g_bp.fns.size()) return;
    if (slot < 0 || slot > 3) return;
    std::string v = clean_arg_name(s ? s : "");
    if (g_bp.fns[g_bp.edit_fn].argn[slot] == v) return;
    g_bp.fns[g_bp.edit_fn].argn[slot] = v;
    touch();
}

extern "C" void engine_bp_fn_set_name(const char* s) {
    if (!g_bp.open || g_bp.edit_fn < 0 || g_bp.edit_fn >= (int)g_bp.fns.size()) return;
    std::string cleaned = clean_fn_name(s ? s : "");
    if (cleaned.empty()) return;
    int idx = g_bp.edit_fn;
    std::string want = unique_fn_name(g_bp, cleaned, idx);
    if (want == g_bp.fns[idx].name) return;
    flush_active(g_bp);
    std::string old = g_bp.fns[idx].name;
    g_bp.fns[idx].name = want;
    retarget_name(g_bp.event.nodes, old, want, false);
    for (int j = 0; j < (int)g_bp.fns.size(); ++j)
        retarget_name(g_bp.fns[j].g.nodes, old, want, j == idx);
    reload_active(g_bp);
    touch();
}

extern "C" int engine_bp_add_call(const char* name, int x, int y) {
    if (!g_bp.open || !name) return -1;
    int fi = find_fn_index(g_bp, name);
    if (fi < 0) return -1;
    int idx = engine_bp_add_node(BP_CALL, x, y);
    if (idx < 0) return -1;
    g_bp.nodes[idx].name = g_bp.fns[fi].name;
    g_bp.nodes[idx].op = g_bp.fns[fi].argc;
    touch();
    return idx;
}

extern "C" void engine_bp_call_set_fn(int node, int fnIndex) {
    BpNode* n = node_at(node);
    if (!n || n->kind != BP_CALL) return;
    if (fnIndex < 0 || fnIndex >= (int)g_bp.fns.size()) return;
    int want = g_bp.fns[fnIndex].argc;
    std::string name = g_bp.fns[fnIndex].name;
    if (n->name == name && clamp_argc(n->op) == want) return;
    int old = clamp_argc(n->op);
    if (old != want) remap_node_links(g_bp.links, node, BP_CALL, old, want);
    n->name = name;
    n->op = want;
    prune_bad_links(g_bp.nodes, g_bp.links);
    touch();
}

void engine_bp_share_push(int handle) {
    g_bpShareStack.push_back(g_bpShareKey);
    g_bpShareKey = handle > 0 ? -handle : -1;
}

void engine_bp_share_pop() {
    if (g_bpShareStack.empty()) {
        g_bpShareKey = 0;
        return;
    }
    g_bpShareKey = g_bpShareStack.back();
    g_bpShareStack.pop_back();
}

extern "C" void engine_bp_values_forget(int id) {
    if (id > 0) g_bpHeld.erase(id);
}

extern "C" void engine_bp_values_clear() { g_bpHeld.clear(); }

static int value_owner(int slot) {
    if (g_bpShareKey != 0) return g_bpShareKey;
    if (!active_slot(slot)) return 0;
    int id = g.entities[slot].id;
    if (id <= 0) return 0;
    return id;
}

static BpHeld* held_mut(int slot, int index) {
    if (index < 0 || index > 31) return nullptr;
    int id = value_owner(slot);
    if (id == 0) return nullptr;
    std::vector<BpHeld>& vec = g_bpHeld[id];
    if ((int)vec.size() <= index) vec.resize(index + 1);
    return &vec[index];
}

extern "C" float engine_bp_value_get(int slot, int index, float fallback) {
    if (index < 0 || index > 31) return fallback;
    int id = value_owner(slot);
    if (id == 0) return fallback;
    auto it = g_bpHeld.find(id);
    if (it == g_bpHeld.end() || index >= (int)it->second.size()) return fallback;
    const BpHeld& h = it->second[index];
    if (!h.hasF) return fallback;
    return h.f;
}

extern "C" void engine_bp_value_set(int slot, int index, float v) {
    BpHeld* h = held_mut(slot, index);
    if (!h) return;
    h->hasF = true;
    h->f = v;
}

extern "C" int engine_bp_value_get_bool(int slot, int index, int fallback) {
    if (index < 0 || index > 31) return fallback ? 1 : 0;
    int id = value_owner(slot);
    if (id == 0) return fallback ? 1 : 0;
    auto it = g_bpHeld.find(id);
    if (it == g_bpHeld.end() || index >= (int)it->second.size()) return fallback ? 1 : 0;
    const BpHeld& h = it->second[index];
    if (!h.hasB) return fallback ? 1 : 0;
    return h.b ? 1 : 0;
}

extern "C" void engine_bp_value_set_bool(int slot, int index, int v) {
    BpHeld* h = held_mut(slot, index);
    if (!h) return;
    h->hasB = true;
    h->b = v ? 1 : 0;
}

extern "C" const char* engine_bp_value_get_str(int slot, int index, const char* fallback) {
    if (!fallback) fallback = "";
    if (index < 0 || index > 31) return fallback;
    int id = value_owner(slot);
    if (id == 0) return fallback;
    auto it = g_bpHeld.find(id);
    if (it == g_bpHeld.end() || index >= (int)it->second.size()) return fallback;
    const BpHeld& h = it->second[index];
    if (!h.hasS) return fallback;
    return h.s.c_str();
}

extern "C" void engine_bp_value_set_str(int slot, int index, const char* v) {
    BpHeld* h = held_mut(slot, index);
    if (!h) return;
    h->hasS = true;
    h->s = v ? v : "";
    if (h->s.size() > 200) h->s.resize(200);
}

extern "C" int engine_bp_var_count() { return g_bp.open ? (int)g_bp.vars.size() : 0; }

extern "C" const char* engine_bp_var_name(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return "";
    return g_bp.vars[i].name.c_str();
}

extern "C" int engine_bp_var_type(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return BP_FLOAT;
    return g_bp.vars[i].type;
}

extern "C" const char* engine_bp_var_num(int i) {
    g_bpRet = "0.0";
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return g_bpRet.c_str();
    g_bpRet = nxa_float(g_bp.vars[i].num);
    return g_bpRet.c_str();
}

extern "C" const char* engine_bp_var_text(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return "";
    return g_bp.vars[i].text.c_str();
}

extern "C" int engine_bp_var_bool(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return 0;
    return g_bp.vars[i].num != 0.f ? 1 : 0;
}

extern "C" int engine_bp_var_find(const char* name) {
    if (!g_bp.open || !name) return -1;
    return find_var_index(g_bp, name);
}

extern "C" int engine_bp_add_var(int type) {
    if (!g_bp.open || g_bp.vars.size() >= 32) return -1;
    BpVar v;
    v.type = clamp_vtype(type);
    v.name = unique_var_name(g_bp, "Value", -1);
    g_bp.vars.push_back(std::move(v));
    touch();
    return (int)g_bp.vars.size() - 1;
}

extern "C" void engine_bp_remove_var(int i) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return;
    flush_active(g_bp);
    std::string name = g_bp.vars[i].name;
    strip_var_nodes(g_bp.event.nodes, g_bp.event.links, name);
    for (BpFn& f : g_bp.fns)
        strip_var_nodes(f.g.nodes, f.g.links, name);
    g_bp.vars.erase(g_bp.vars.begin() + i);
    reload_active(g_bp);
    touch();
}

extern "C" void engine_bp_var_set_name(int i, const char* s) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return;
    std::string cleaned = clean_fn_name(s ? s : "");
    if (cleaned.empty()) return;
    std::string want = unique_var_name(g_bp, cleaned, i);
    if (want == g_bp.vars[i].name) return;
    flush_active(g_bp);
    std::string old = g_bp.vars[i].name;
    g_bp.vars[i].name = want;
    rename_var_nodes(g_bp.event.nodes, old, want);
    for (BpFn& f : g_bp.fns) rename_var_nodes(f.g.nodes, old, want);
    reload_active(g_bp);
    touch();
}

extern "C" void engine_bp_var_set_type(int i, int type) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return;
    type = clamp_vtype(type);
    if (g_bp.vars[i].type == type) return;
    flush_active(g_bp);
    g_bp.vars[i].type = type;
    std::string name = g_bp.vars[i].name;
    retype_var_nodes(g_bp.event.nodes, g_bp.event.links, name, type);
    for (BpFn& f : g_bp.fns) retype_var_nodes(f.g.nodes, f.g.links, name, type);
    reload_active(g_bp);
    touch();
}

extern "C" int engine_bp_var_set_num(int i, const char* s) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return 0;
    float v = 0.f;
    if (!parse_f(s, &v)) return 0;
    if (g_bp.vars[i].num == v) return 1;
    g_bp.vars[i].num = v;
    touch();
    return 1;
}

extern "C" void engine_bp_var_set_text(int i, const char* s) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return;
    std::string v = s ? s : "";
    if (v.size() > 200) v.resize(200);
    if (g_bp.vars[i].text == v) return;
    g_bp.vars[i].text = std::move(v);
    touch();
}

extern "C" void engine_bp_var_set_bool(int i, int v) {
    if (!g_bp.open || i < 0 || i >= (int)g_bp.vars.size()) return;
    float n = v ? 1.f : 0.f;
    if (g_bp.vars[i].num == n) return;
    g_bp.vars[i].num = n;
    touch();
}

extern "C" int engine_bp_add_get(int varIndex, int x, int y) {
    if (!g_bp.open || varIndex < 0 || varIndex >= (int)g_bp.vars.size()) return -1;
    int idx = engine_bp_add_node(BP_GETVAR, x, y);
    if (idx < 0) return -1;
    g_bp.nodes[idx].name = g_bp.vars[varIndex].name;
    g_bp.nodes[idx].op = g_bp.vars[varIndex].type;
    touch();
    return idx;
}

extern "C" int engine_bp_add_set(int varIndex, int x, int y) {
    if (!g_bp.open || varIndex < 0 || varIndex >= (int)g_bp.vars.size()) return -1;
    int idx = engine_bp_add_node(BP_SETVAR, x, y);
    if (idx < 0) return -1;
    g_bp.nodes[idx].name = g_bp.vars[varIndex].name;
    g_bp.nodes[idx].op = g_bp.vars[varIndex].type;
    g_bp.nodes[idx].a = g_bp.vars[varIndex].num;
    g_bp.nodes[idx].text = g_bp.vars[varIndex].text;
    g_bp.nodes[idx].b = 1.f;
    touch();
    return idx;
}
