/* Jolt Physics for RENDER 3D scenes. Included from engine.cpp.
   Scene coords: +Y down the screen, +Z into the screen.
   Jolt coords:  +Y up, +Z out of the screen (right-handed). */

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <thread>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Geometry/Triangle.h>

JPH_SUPPRESS_WARNINGS

/* Implementation lives in the TU anonymous namespace (same as HitCache / solid_depth). */
namespace {

using namespace JPH;

constexpr uint32_t kJoltInvalid = 0xffffffffu;

namespace JoltLayers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}

class JoltObjectLayerPairFilter : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override {
        switch (a) {
        case JoltLayers::NON_MOVING: return b == JoltLayers::MOVING;
        case JoltLayers::MOVING:     return true;
        default:                     return false;
        }
    }
};

namespace JoltBPLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint NUM_LAYERS(2);
}

class JoltBPLayerInterface final : public BroadPhaseLayerInterface {
public:
    JoltBPLayerInterface() {
        mMap[JoltLayers::NON_MOVING] = JoltBPLayers::NON_MOVING;
        mMap[JoltLayers::MOVING] = JoltBPLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return JoltBPLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        return mMap[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
        case (BroadPhaseLayer::Type)JoltBPLayers::NON_MOVING: return "NON_MOVING";
        case (BroadPhaseLayer::Type)JoltBPLayers::MOVING:     return "MOVING";
        default: return "INVALID";
        }
    }
#endif
private:
    BroadPhaseLayer mMap[JoltLayers::NUM_LAYERS];
};

class JoltObjectVsBroadPhaseFilter : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
        case JoltLayers::NON_MOVING: return inLayer2 == JoltBPLayers::MOVING;
        case JoltLayers::MOVING:     return true;
        default:                     return false;
        }
    }
};

struct JoltWorld {
    bool inited = false;
    TempAllocatorImpl* temp = nullptr;
    JobSystemThreadPool* jobs = nullptr;
    PhysicsSystem* system = nullptr;
    JoltBPLayerInterface bpLayers;
    JoltObjectVsBroadPhaseFilter objVsBp;
    JoltObjectLayerPairFilter objPair;
};

static JoltWorld g_jolt;
static bool g_joltTypes = false;
/* The first frames after Play often include a shader-compile hitch. Those
   must not fast-forward the pile, or every launch settles differently. */
static int g_joltHold = 0;

static void jolt_ensure_types() {
    if (g_joltTypes) return;
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();
    g_joltTypes = true;
}

static bool jolt_active() { return g_jolt.system != nullptr; }

static BodyID jolt_id(const Entity& e) {
    return BodyID(e.joltBody);
}

static bool jolt_has(const Entity& e) {
    return e.joltBody != kJoltInvalid;
}

/* Scene center (px) → Jolt meters. */
static RVec3 scene_to_jolt(float cx, float cy, float cz, float ppm) {
    return RVec3(cx / ppm, -cy / ppm, -cz / ppm);
}

static void jolt_to_scene(RVec3Arg p, float ppm, float& cx, float& cy, float& cz) {
    cx = (float)p.GetX() * ppm;
    cy = -(float)p.GetY() * ppm;
    cz = -(float)p.GetZ() * ppm;
}

/* Match orient_matrix: scene roll/pitch/yaw → Jolt quaternion.
   Scene rotation is applied in a Y-down / Z-into frame; convert the resulting
   basis into Jolt's Y-up frame by flipping Y and Z on each basis vector. */
static Quat scene_orient_to_jolt(int rotX, int rotY, int rotDeg) {
    float m[16];
    orient_matrix(m, rotX, rotY, rotDeg);
    /* Column vectors of the scene rotation (column-major mat4). */
    Vec3 sx(m[0], m[1], m[2]);
    Vec3 sy(m[4], m[5], m[6]);
    Vec3 sz(m[8], m[9], m[10]);
    /* Flip Y and Z components to enter Jolt space. */
    auto flip = [](Vec3Arg v) { return Vec3(v.GetX(), -v.GetY(), -v.GetZ()); };
    Vec3 jx = flip(sx);
    Vec3 jy = flip(sy);
    Vec3 jz = flip(sz);
    /* Re-orthonormalize in case of float drift. */
    jx = jx.Normalized();
    jz = jx.Cross(jy).Normalized();
    jy = jz.Cross(jx);
    Mat44 mat = Mat44::sIdentity();
    mat.SetAxisX(jx);
    mat.SetAxisY(jy);
    mat.SetAxisZ(jz);
    return mat.GetQuaternion().Normalized();
}

static void jolt_orient_to_scene(QuatArg q, int& rotX, int& rotY, int& rotDeg) {
    Mat44 mat = Mat44::sRotation(q);
    Vec3 jx = mat.GetAxisX();
    Vec3 jy = mat.GetAxisY();
    Vec3 jz = mat.GetAxisZ();
    auto flip = [](Vec3Arg v) { return Vec3(v.GetX(), -v.GetY(), -v.GetZ()); };
    Vec3 sx = flip(jx);
    Vec3 sy = flip(jy);
    Vec3 sz = flip(jz);
    float m[16];
    mat4_ident(m);
    m[0] = sx.GetX(); m[1] = sx.GetY(); m[2] = sx.GetZ();
    m[4] = sy.GetX(); m[5] = sy.GetY(); m[6] = sy.GetZ();
    m[8] = sz.GetX(); m[9] = sz.GetY(); m[10] = sz.GetZ();
    orient_extract(m, &rotX, &rotY, &rotDeg);
}

static EMotionType jolt_motion(int bodyType) {
    if (bodyType == 0) return EMotionType::Static;
    if (bodyType == 2) return EMotionType::Kinematic;
    return EMotionType::Dynamic;
}

static ObjectLayer jolt_layer(int bodyType) {
    return bodyType == 0 ? JoltLayers::NON_MOVING : JoltLayers::MOVING;
}

static ShapeRefC jolt_make_shape(const Entity& e, float ppm) {
    int w = e.w < 4 ? 4 : e.w;
    int h = e.h < 4 ? 4 : e.h;
    int d = solid_depth(e);
    if (d < 4) d = w;

    /* Model mesh hitboxes: compound of XY pieces extruded in Z, or triangle mesh. */
    if (e.hitbox > 0 && e.kind == "model") {
        const HitCache& cache = hitbox_cache(e);
        if (!cache.pieces.empty()) {
            StaticCompoundShapeSettings compound;
            compound.SetEmbedded();
            float hz = (float)d * 0.5f / ppm;
            if (hz < 0.05f) hz = 0.05f;
            for (const HitPiece& p : cache.pieces) {
                float hx = p.hw / ppm;
                float hy = p.hh / ppm;
                if (hx < 0.01f) hx = 0.01f;
                if (hy < 0.01f) hy = 0.01f;
                /* Piece offsets are scene px (Y down); flip Y into Jolt. */
                Vec3 offset(p.cx / ppm, -p.cy / ppm, 0.f);
                BoxShapeSettings box(Vec3(hx, hy, hz));
                box.SetEmbedded();
                compound.AddShape(offset, Quat::sIdentity(), box.Create().Get());
            }
            ShapeSettings::ShapeResult r = compound.Create();
            if (r.IsValid()) return r.Get();
        } else if (!cache.skin.empty() && e.bodyType == 0) {
            /* Static triangle mesh from skin (scene offsets → Jolt). */
            TriangleList tris;
            for (size_t i = 0; i + 8 < cache.skin.size(); i += 9) {
                Float3 v0(cache.skin[i] / ppm, -cache.skin[i + 1] / ppm, -cache.skin[i + 2] / ppm);
                Float3 v1(cache.skin[i + 3] / ppm, -cache.skin[i + 4] / ppm, -cache.skin[i + 5] / ppm);
                Float3 v2(cache.skin[i + 6] / ppm, -cache.skin[i + 7] / ppm, -cache.skin[i + 8] / ppm);
                tris.push_back(Triangle(v0, v1, v2));
            }
            if (!tris.empty()) {
                MeshShapeSettings mesh(tris);
                mesh.SetEmbedded();
                ShapeSettings::ShapeResult r = mesh.Create();
                if (r.IsValid()) return r.Get();
            }
        }
    }

    if (e.kind == "sphere") {
        float rad = (float)(w < h ? w : h) * 0.5f / ppm;
        if (rad < 0.05f) rad = 0.05f;
        return new SphereShape(rad);
    }
    if (e.kind == "capsule") {
        float rad = (float)w * 0.5f / ppm;
        if (rad < 0.05f) rad = 0.05f;
        float halfCyl = ((float)h * 0.5f / ppm) - rad;
        if (halfCyl < 0.01f) {
            /* Too short for a capsule cylinder — use a sphere. */
            return new SphereShape(rad);
        }
        return new CapsuleShape(halfCyl, rad);
    }
    if (e.kind == "cylinder") {
        float rad = (float)w * 0.5f / ppm;
        float halfH = (float)h * 0.5f / ppm;
        if (rad < 0.05f) rad = 0.05f;
        if (halfH < 0.05f) halfH = 0.05f;
        return new CylinderShape(halfH, rad);
    }

    /* cube / model AABB / extruded rect */
    float hx = (float)w * 0.5f / ppm;
    float hy = (float)h * 0.5f / ppm;
    float hz = (float)d * 0.5f / ppm;
    if (hx < 0.05f) hx = 0.05f;
    if (hy < 0.05f) hy = 0.05f;
    if (hz < 0.05f) hz = 0.05f;
    return new BoxShape(Vec3(hx, hy, hz));
}

static void jolt_create_body(int slot) {
    if (!g_jolt.system || !active_slot(slot)) return;
    Entity& e = g.entities[slot];
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;

    int w = e.w < 4 ? 4 : e.w;
    int h = e.h < 4 ? 4 : e.h;
    int d = solid_depth(e);
    if (d < 4) d = w;
    float cx = e.x + w * 0.5f;
    float cy = e.y + h * 0.5f;
    float cz = e.z + d * 0.5f;

    ShapeRefC shape = jolt_make_shape(e, ppm);
    if (shape == nullptr) return;

    EMotionType motion = jolt_motion(e.bodyType);
    ObjectLayer layer = jolt_layer(e.bodyType);
    Quat rot = scene_orient_to_jolt(e.rotX, e.rotY, e.rotDeg);

    BodyCreationSettings settings(shape, scene_to_jolt(cx, cy, cz, ppm), rot, motion, layer);
    settings.mFriction = e.friction;
    settings.mRestitution = e.restitution;
    settings.mGravityFactor = e.gravityScale;
    settings.mUserData = (uint64)(slot + 1);
    if (e.density > 0.f && motion == EMotionType::Dynamic) {
        settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = e.density * shape->GetVolume();
        if (settings.mMassPropertiesOverride.mMass < 0.01f)
            settings.mMassPropertiesOverride.mMass = 0.01f;
    }
    if (e.fixedRotation) {
        settings.mAllowedDOFs = EAllowedDOFs::TranslationX | EAllowedDOFs::TranslationY | EAllowedDOFs::TranslationZ;
    }

    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    BodyID id = bi.CreateAndAddBody(settings, motion == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate);
    if (id.IsInvalid()) {
        e.joltBody = kJoltInvalid;
        return;
    }
    e.joltBody = id.GetIndexAndSequenceNumber();
}

static void jolt_destroy_body(Entity& e) {
    if (!g_jolt.system || !jolt_has(e)) return;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    BodyID id = jolt_id(e);
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    e.joltBody = kJoltInvalid;
}

static void jolt_rebuild_body(int slot) {
    if (!g_jolt.system || !active_slot(slot)) return;
    Entity& e = g.entities[slot];
    jolt_destroy_body(e);
    if (e.active && e.hasBody) jolt_create_body(slot);
}

static void jolt_begin() {
    jolt_ensure_types();
    if (g_jolt.system) return;

    g_jolt.temp = new TempAllocatorImpl(8 * 1024 * 1024);
    /* Extra worker threads (main thread also runs jobs). Cap so small scenes
       do not pay more in scheduling than they gain. */
    int workers = (int)std::thread::hardware_concurrency();
    if (workers > 1) workers -= 1;
    if (workers < 1) workers = 1;
    if (workers > 6) workers = 6;
    g_jolt.jobs = new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, workers);
    g_jolt.system = new PhysicsSystem();
    const uint cMaxBodies = 4096;
    const uint cMaxBodyPairs = 4096;
    const uint cMaxContacts = 4096;
    g_jolt.system->Init(cMaxBodies, 0, cMaxBodyPairs, cMaxContacts,
                        g_jolt.bpLayers, g_jolt.objVsBp, g_jolt.objPair);

    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    /* Scene +Y gravity pulls down the screen → negative Jolt Y. */
    g_jolt.system->SetGravity(Vec3(g.gravityX / ppm, -g.gravityY / ppm, 0.f));

    for (int s = 0; s < (int)g.entities.size(); ++s) {
        g.entities[s].joltBody = kJoltInvalid;
        g.entities[s].body = nullptr;
        if (g.entities[s].active && g.entities[s].hasBody)
            jolt_create_body(s);
    }
    g_jolt.system->OptimizeBroadPhase();
    g_jolt.inited = true;
    g_joltHold = 2;
}

static void jolt_end() {
    if (!g_jolt.system) return;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    for (Entity& e : g.entities) {
        if (jolt_has(e)) {
            BodyID id = jolt_id(e);
            bi.RemoveBody(id);
            bi.DestroyBody(id);
            e.joltBody = kJoltInvalid;
        }
    }
    delete g_jolt.system;
    g_jolt.system = nullptr;
    delete g_jolt.jobs;
    g_jolt.jobs = nullptr;
    delete g_jolt.temp;
    g_jolt.temp = nullptr;
    g_jolt.inited = false;
    g_joltHold = 0;
}

/* One or two fixed 1/60 s slices. Frame-time jitter stays on one slice, a
   30 fps frame gets two, and a hitch cannot run the sim ahead. */
static void jolt_step(float dt) {
    if (!g_jolt.system) return;
    int steps = 1;
    if (g_joltHold > 0) {
        g_joltHold--;
    } else if (dt >= 1.5f / 60.f) {
        steps = 2;
    }
    const float h = 1.f / 60.f;
    for (int i = 0; i < steps; ++i)
        g_jolt.system->Update(h, 1, g_jolt.temp, g_jolt.jobs);

    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    /* NoLock is safe here: Update finished on this thread, and scripts run after. */
    BodyInterface& bi = g_jolt.system->GetBodyInterfaceNoLock();

    for (int s = 0; s < (int)g.entities.size(); ++s) {
        Entity& e = g.entities[s];
        if (!e.active || !e.hasBody || !jolt_has(e)) continue;
        if (e.bodyType == 0) continue;

        BodyID id = jolt_id(e);
        /* Sleeping bodies have not moved since the last awake writeback. */
        if (!bi.IsActive(id)) continue;

        RVec3 p = bi.GetPosition(id);
        float cx, cy, cz;
        jolt_to_scene(p, ppm, cx, cy, cz);
        int w = e.w < 4 ? 4 : e.w;
        int h = e.h < 4 ? 4 : e.h;
        int d = solid_depth(e);
        if (d < 4) d = w;
        int nx = (int)lroundf(cx - w * 0.5f);
        int ny = (int)lroundf(cy - h * 0.5f);
        int nz = (int)lroundf(cz - d * 0.5f);

        /* Direct write — avoid set_x/set_y (physics_apply) on every body. */
        int dx = nx - e.x;
        int dy = ny - e.y;
        int dz = nz - e.z;
        if (dx != 0 || dy != 0 || dz != 0) {
            e.x = nx;
            e.y = ny;
            e.z = nz;
            if (dx != 0 || dy != 0) entity_propagate_delta(s, dx, dy);
            if (dz != 0) entity_propagate_dz(s, dz);
        }

        /* Spheres look the same at any yaw; skip expensive quat→euler. */
        if (!e.fixedRotation && e.kind != "sphere") {
            Quat q = bi.GetRotation(id);
            int rx = 0, ry = 0, rz = 0;
            jolt_orient_to_scene(q, rx, ry, rz);
            e.rotX = rx;
            e.rotY = ry;
            e.rotDeg = rz;
        }
    }
}

static void jolt_apply_entity(Entity& e) {
    if (!g_jolt.system || !e.hasBody || !jolt_has(e)) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    int w = e.w < 4 ? 4 : e.w;
    int h = e.h < 4 ? 4 : e.h;
    int d = solid_depth(e);
    if (d < 4) d = w;
    float cx = e.x + w * 0.5f;
    float cy = e.y + h * 0.5f;
    float cz = e.z + d * 0.5f;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    BodyID id = jolt_id(e);
    bi.SetPositionAndRotation(id, scene_to_jolt(cx, cy, cz, ppm),
                              scene_orient_to_jolt(e.rotX, e.rotY, e.rotDeg),
                              EActivation::Activate);
    bi.SetLinearVelocity(id, Vec3::sZero());
    bi.SetAngularVelocity(id, Vec3::sZero());
}

static void jolt_set_gravity(float gx, float gy) {
    if (!g_jolt.system) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    g_jolt.system->SetGravity(Vec3(gx / ppm, -gy / ppm, 0.f));
}

/* Force/impulse/velocity: scene px/s² and px/s → Jolt (flip Y/Z). */
static void jolt_apply_force(Entity& e, float fx, float fy, float fz) {
    if (!g_jolt.system || !jolt_has(e)) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    bi.AddForce(jolt_id(e), Vec3(fx / ppm, -fy / ppm, -fz / ppm));
}

static void jolt_apply_impulse(Entity& e, float ix, float iy, float iz) {
    if (!g_jolt.system || !jolt_has(e)) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    bi.AddImpulse(jolt_id(e), Vec3(ix / ppm, -iy / ppm, -iz / ppm));
}

static void jolt_set_velocity(Entity& e, float vx, float vy, float vz) {
    if (!g_jolt.system || !jolt_has(e)) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    bi.SetLinearVelocity(jolt_id(e), Vec3(vx / ppm, -vy / ppm, -vz / ppm));
}

static void jolt_get_velocity(const Entity& e, float& vx, float& vy, float& vz) {
    vx = vy = vz = 0.f;
    if (!g_jolt.system || !jolt_has(e)) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    Vec3 v = g_jolt.system->GetBodyInterface().GetLinearVelocity(jolt_id(e));
    vx = v.GetX() * ppm;
    vy = -v.GetY() * ppm;
    vz = -v.GetZ() * ppm;
}

static void jolt_set_body_type(Entity& e, int t) {
    if (!g_jolt.system || !jolt_has(e)) return;
    BodyInterface& bi = g_jolt.system->GetBodyInterface();
    bi.SetMotionType(jolt_id(e), jolt_motion(t), EActivation::Activate);
    bi.SetObjectLayer(jolt_id(e), jolt_layer(t));
}

static void jolt_set_friction(Entity& e, float v) {
    if (!g_jolt.system || !jolt_has(e)) return;
    BodyLockWrite lock(g_jolt.system->GetBodyLockInterface(), jolt_id(e));
    if (lock.Succeeded()) lock.GetBody().SetFriction(v);
}

static void jolt_set_restitution(Entity& e, float v) {
    if (!g_jolt.system || !jolt_has(e)) return;
    BodyLockWrite lock(g_jolt.system->GetBodyLockInterface(), jolt_id(e));
    if (lock.Succeeded()) lock.GetBody().SetRestitution(v);
}

static void jolt_set_gravity_scale(Entity& e, float v) {
    if (!g_jolt.system || !jolt_has(e)) return;
    g_jolt.system->GetBodyInterface().SetGravityFactor(jolt_id(e), v);
}

static void jolt_set_fixed_rotation(Entity& e, int on) {
    /* DOFs are set at creation; rebuild to apply. */
    (void)on;
    if (!g_jolt.system || !jolt_has(e)) return;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (&g.entities[s] == &e) { jolt_rebuild_body(s); return; }
    }
}

} // namespace
