// b2world.h - pointer-free port of the Box2DFlash 2.0.x engine embedded in
// Red Ball 1 (practice-hack and tournament builds are byte-identical here).
//
// Design:
//   * All mutable simulation state lives inside `World` in fixed arrays linked
//     by int indices (-1 = null). Copying a World (memcpy / operator=) is a
//     complete snapshot, which is what the search needs.
//   * Immutable shape geometry (polygon vertices, normals, OBB...) lives in a
//     shared GeomTable that snapshots merely point at.
//   * Per-step scratch (islands, contact constraints) is thread_local and never
//     part of a snapshot.
#pragma once
#include "b2math.h"
#include <vector>
#include <cstring>

namespace rb {

// ----------------------------------------------------------------- capacities
// Storage capacities. The engine semantics (proxy id assignment, pair order)
// are identical to the original as long as these are never exceeded; hash
// table size and pair-slot indices are unobservable. Overflow aborts.
constexpr int CAP_BODIES = 96;
constexpr int CAP_SHAPES = 192;
constexpr int CAP_CONTACTS = 160;
constexpr int CAP_JOINTS = 48;
constexpr int CAP_PROXIES = 192;
constexpr int CAP_PAIRS = 1024;              // power of two
constexpr int CAP_PAIR_TABLE = CAP_PAIRS;
constexpr int CAP_PAIR_BUFFER = CAP_PAIRS;
constexpr int CAP_PLAYER_CONTACTS = 32;
constexpr int MAX_POLY_VERTS = settings::maxPolygonVertices;

constexpr uint32_t NULL_PROXY = 65535;       // b2Pair.b2_nullProxy
constexpr uint32_t NULL_PAIR = 65535;        // b2Pair.b2_nullPair
constexpr uint32_t NULL_EDGE = 65535;
constexpr uint32_t BP_INVALID = 65535;       // b2BroadPhase.b2_invalid
constexpr int32_t PAIRDATA_NONE = -1;        // pair.userData == null
constexpr int32_t PAIRDATA_NULLCONTACT = -2; // pair.userData == m_nullContact

[[noreturn]] void fatal(const char* msg);

// ----------------------------------------------------------------- shapes
enum ShapeType : int32_t { e_circleShape = 0, e_polygonShape = 1 };

struct FilterData {
    uint32_t categoryBits = 1;
    uint32_t maskBits = 65535;
    int32_t groupIndex = 0;
};

struct ShapeDef {
    int32_t type = -1;
    double friction = 0.2;
    double restitution = 0.0;
    double density = 0.0;
    bool isSensor = false;
    FilterData filter;
    // b2CircleDef
    double radius = 1;
    Vec2 localPosition;
    // b2PolygonDef
    int32_t vertexCount = 0;
    Vec2 vertices[MAX_POLY_VERTS];
};

struct OBB {
    Mat22 R;
    Vec2 center, extents;
};

// Immutable geometry derived from a ShapeDef (b2CircleShape/b2PolygonShape ctors)
struct Geom {
    int32_t type = -1;
    Vec2 localPosition;
    double radius = 0;
    int32_t vertexCount = 0;
    Vec2 vertices[MAX_POLY_VERTS];
    Vec2 normals[MAX_POLY_VERTS];
    Vec2 coreVertices[MAX_POLY_VERTS];
    Vec2 centroid;
    OBB obb;
};

struct GeomTable {
    std::vector<Geom> geoms;
    int32_t Add(const ShapeDef& def);  // computes geometry exactly like the AS3 ctors
    const Geom& operator[](int32_t i) const { return geoms[(size_t)i]; }
};

struct Shape {
    int32_t geom = -1;
    int32_t type = -1;
    int32_t body = -1;
    int32_t next = -1;
    double sweepRadius = 0;
    double density = 0, friction = 0, restitution = 0;
    FilterData filter;
    bool isSensor = false;
    uint32_t proxyId = NULL_PROXY;
};

struct MassData {
    double mass = 0;
    Vec2 center;
    double I = 0;
};

// ----------------------------------------------------------------- manifold
// b2ContactID key layout: referenceEdge | incidentEdge<<8 | incidentVertex<<16 | flip<<24
inline void id_setReferenceEdge(uint32_t& k, uint32_t v) { k = (k & 4294967040u) | (v & 255u); }
inline void id_setIncidentEdge(uint32_t& k, uint32_t v) { k = (k & 4294902015u) | ((v << 8) & 65280u); }
inline void id_setIncidentVertex(uint32_t& k, uint32_t v) { k = (k & 4278255615u) | ((v << 16) & 16711680u); }
inline void id_setFlip(uint32_t& k, uint32_t v) { k = (k & 16777215u) | ((v << 24) & 4278190080u); }

struct ManifoldPoint {
    Vec2 localPoint1, localPoint2;
    double separation = NUM_NAN;       // AS3 uninitialized Number is NaN
    double normalImpulse = NUM_NAN;
    double tangentImpulse = NUM_NAN;
    uint32_t key = 0;
};

struct Manifold {
    ManifoldPoint points[settings::maxManifoldPoints];
    Vec2 normal;
    int32_t pointCount = 0;
};

// ----------------------------------------------------------------- contacts
enum ContactKind : int32_t { CK_NONE = 0, CK_CIRCLE = 1, CK_POLYCIRCLE = 2, CK_POLYGON = 3 };

constexpr uint32_t CF_NONSOLID = 1, CF_SLOW = 2, CF_ISLAND = 4, CF_TOI = 8;

struct ContactEdge {           // edge ref = contactIndex*2 + nodeIndex
    int32_t other = -1;        // body
    int32_t prev = -1, next = -1;
};

struct Contact {
    int32_t kind = CK_NONE;
    int32_t shape1 = -1, shape2 = -1;
    int32_t prev = -1, next = -1;
    ContactEdge node[2];
    double toi = NUM_NAN;
    double friction = 0, restitution = 0;
    uint32_t flags = 0;
    int32_t manifoldCount = 0;
    Manifold manifold;
    int32_t nextFree = -1;
};

// ----------------------------------------------------------------- joints
// Joints are the next milestone; the structural parts (lists, island edges,
// IsConnected) are already wired so Solve() is faithful once they land.
enum JointType : int32_t { JT_UNKNOWN = 0, JT_REVOLUTE, JT_PRISMATIC, JT_DISTANCE, JT_PULLEY, JT_MOUSE, JT_GEAR };

struct JointEdge {             // edge ref = jointIndex*2 + nodeIndex
    int32_t other = -1;
    int32_t prev = -1, next = -1;
};

struct Joint {
    int32_t type = JT_UNKNOWN;
    int32_t body1 = -1, body2 = -1;
    int32_t prev = -1, next = -1;
    JointEdge node[2];
    bool islandFlag = false;
    bool collideConnected = false;
    bool alive = false;
};

// ----------------------------------------------------------------- bodies
constexpr uint32_t BF_FROZEN = 2, BF_ISLAND = 4, BF_SLEEP = 8, BF_ALLOWSLEEP = 16, BF_BULLET = 32,
                   BF_FIXEDROTATION = 64;
constexpr int32_t BT_STATIC = 1, BT_DYNAMIC = 2;

struct BodyDef {
    MassData massData;
    Vec2 position;
    double angle = 0;
    double linearDamping = 0, angularDamping = 0;
    bool allowSleep = true, isSleeping = false, fixedRotation = false, isBullet = false;
    int32_t userTag = -1;
};

struct Body {
    XForm xf;
    Sweep sweep;
    Vec2 linearVelocity;
    Vec2 force;
    double angularVelocity = 0, torque = 0;
    double mass = 0, invMass = 0, I = 0, invI = 0;
    double linearDamping = 0, angularDamping = 0;
    double sleepTime = 0;
    uint32_t flags = 0;
    int32_t type = 0;
    int32_t prev = -1, next = -1;
    int32_t shapeList = -1;
    int32_t shapeCount = 0;
    int32_t contactList = -1;  // ContactEdge ref
    int32_t jointList = -1;    // JointEdge ref
    int32_t userTag = -1;      // game object id (e.g. player)
    bool inWorld = false;      // false after DestroyBody (object stays readable, like AS3)

    bool IsStatic() const { return type == BT_STATIC; }
    bool IsSleeping() const { return (flags & BF_SLEEP) == BF_SLEEP; }
    bool IsBullet() const { return (flags & BF_BULLET) == BF_BULLET; }
    bool IsFrozen() const { return (flags & BF_FROZEN) == BF_FROZEN; }
    void WakeUp() { flags &= ~BF_SLEEP; sleepTime = 0; }
};

// ----------------------------------------------------------------- broadphase
struct Bound {
    uint32_t value = 0, proxyId = 0, stabbingCount = 0;
    bool IsLower() const { return (value & 1) == 0; }
    bool IsUpper() const { return (value & 1) == 1; }
};

struct Proxy {
    uint32_t lowerBounds[2] = {0, 0};
    uint32_t upperBounds[2] = {0, 0};
    uint32_t overlapCount = 0;
    uint32_t timeStamp = 0;
    int32_t userData = -1;  // shape index
    uint32_t GetNext() const { return lowerBounds[0]; }
    void SetNext(uint32_t n) { lowerBounds[0] = n & 65535; }
    bool IsValid() const { return overlapCount != BP_INVALID; }
};

constexpr uint32_t PAIR_BUFFERED = 1, PAIR_REMOVED = 2, PAIR_FINAL = 4;

struct Pair {
    uint32_t proxyId1 = NULL_PROXY, proxyId2 = NULL_PROXY;
    uint32_t status = 0;
    uint32_t next = NULL_PAIR;
    int32_t userData = PAIRDATA_NONE;
};

struct BufferedPair {
    uint32_t proxyId1 = 0, proxyId2 = 0;
};

struct PairManager {
    Pair pairs[CAP_PAIRS];
    uint32_t hashTable[CAP_PAIR_TABLE];
    BufferedPair pairBuffer[CAP_PAIR_BUFFER];
    int32_t pairCount = 0;
    int32_t pairBufferCount = 0;
    uint32_t freePair = 0;
};

struct BroadPhase {
    PairManager pm;
    Proxy proxyPool[CAP_PROXIES];
    Bound bounds[2][2 * CAP_PROXIES];
    uint32_t queryResults[CAP_PROXIES];
    int32_t queryResultCount = 0;
    AABB worldAABB;
    Vec2 quantizationFactor;
    uint32_t freeProxy = 0;
    int32_t proxyCount = 0;
    uint32_t timeStamp = 1;
};

// ----------------------------------------------------------------- listener
// Emulation of the game's MyContactListener, including the original bug in
// Remove(): Array.splice(bodyObject, 1) coerces the object to 0, so it always
// removes index 0 instead of the matching body.
struct PlayerContactListener {
    bool enabled = false;
    int32_t playerBody = -1;
    int32_t count = 0;
    int32_t bodies[CAP_PLAYER_CONTACTS];
};

// ----------------------------------------------------------------- world
struct TimeStep {
    double dt = NUM_NAN;
    double inv_dt = NUM_NAN;
    double dtRatio = NUM_NAN;
    int32_t maxIterations = 0;
    bool warmStarting = false;
    bool positionCorrection = false;
};

struct World {
    const GeomTable* geoms = nullptr;

    int32_t bodyList = -1, contactList = -1, jointList = -1;
    int32_t bodyCount = 0, contactCount = 0, jointCount = 0;
    int32_t groundBody = -1;
    Vec2 gravity;
    bool allowSleep = true;
    bool lock = false;
    double inv_dt0 = 0;
    int32_t positionIterationCount = 0;

    // b2World statics (true for every level in this game)
    bool positionCorrection = true, warmStarting = true, continuousPhysics = true;

    int32_t numBodies = 0;   // bodies are never recycled (AS3 keeps dead objects readable)
    int32_t numShapes = 0;
    int32_t numJoints = 0;
    int32_t freeContact = -1;
    int32_t numContactSlots = 0;

    Body bodies[CAP_BODIES];
    Shape shapes[CAP_SHAPES];
    Contact contacts[CAP_CONTACTS];
    Joint joints[CAP_JOINTS];
    BroadPhase bp;
    PlayerContactListener listener;

    // ---- construction
    void Init(const GeomTable* table, const AABB& worldAABB, const Vec2& gravity, bool doSleep);

    // ---- b2World API
    int32_t CreateBody(const BodyDef& def);
    void DestroyBody(int32_t b);
    void Step(double dt, int32_t iterations);
    int32_t Query(const AABB& aabb, int32_t* out, int32_t maxCount);  // returns shape indices
    void Refilter(int32_t shape);

    // ---- b2Body API
    int32_t CreateShape(int32_t body, int32_t geom, const ShapeDef& def);
    void SetMassFromShapes(int32_t body);
    void ApplyForce(int32_t body, const Vec2& force, const Vec2& point);
    void ApplyImpulse(int32_t body, const Vec2& impulse, const Vec2& point);
    bool IsConnected(int32_t body, int32_t other) const;

    // ---- shape helpers
    bool ShapeTestPoint(int32_t shape, const XForm& xf, const Vec2& p) const;

    // ---- internals (public for testing)
    void SynchronizeTransform(int32_t b);
    bool SynchronizeShapes(int32_t b);
    void Advance(int32_t b, double t);
    void Solve(const TimeStep& step);
    void SolveTOI(const TimeStep& step);
    void ContactUpdate(int32_t c);
    void ContactEvaluate(int32_t c);
    void CollideAll();
    int32_t PairAdded(int32_t shapeA, int32_t shapeB);
    void PairRemoved(int32_t userData);
    void DestroyContact(int32_t c);

    // broadphase
    void BP_Init(const AABB& worldAABB);
    uint32_t BP_CreateProxy(const AABB& aabb, int32_t shape);
    void BP_DestroyProxy(uint32_t proxyId);
    void BP_MoveProxy(uint32_t proxyId, const AABB& aabb);
    bool BP_InRange(const AABB& aabb) const;
    int32_t BP_QueryAABB(const AABB& aabb, int32_t* out, int32_t maxCount);
    void BP_Commit();

    // shape <-> broadphase
    void Shape_CreateProxy(int32_t s, const XForm& xf);
    void Shape_DestroyProxy(int32_t s);
    bool Shape_Synchronize(int32_t s, const XForm& xf1, const XForm& xf2);
    void Shape_RefilterProxy(int32_t s, const XForm& xf);

    // listener
    void ListenerAdd(int32_t shape1, int32_t shape2);
    void ListenerRemove(int32_t shape1, int32_t shape2);

   private:
    void BP_Query(uint32_t& lowerOut, uint32_t& upperOut, uint32_t lowerValue, uint32_t upperValue,
                  Bound* bounds, uint32_t boundCount, int axis);
    void BP_IncrementOverlapCount(uint32_t proxyId);
    void BP_IncrementTimeStamp();
    void BP_ComputeBounds(uint32_t lower[2], uint32_t upper[2], const AABB& aabb) const;
    bool BP_TestOverlap(const uint32_t lowerValues[2], const uint32_t upperValues[2], const Proxy& p) const;
    void PM_AddBufferedPair(uint32_t id1, uint32_t id2);
    void PM_RemoveBufferedPair(uint32_t id1, uint32_t id2);
    void PM_Commit();
    Pair* PM_AddPair(uint32_t id1, uint32_t id2);
    int32_t PM_RemovePair(uint32_t id1, uint32_t id2);
    Pair* PM_Find(uint32_t id1, uint32_t id2);
    Pair* PM_FindHash(uint32_t id1, uint32_t id2, uint32_t hash);
    int32_t CreateContact(int32_t shape1, int32_t shape2);
    int32_t AllocContact();
};

// collision / geometry free functions (exposed for tests)
void ComputeAABB(const Geom& g, AABB& aabb, const XForm& xf);
void ComputeSweptAABB(const Geom& g, AABB& aabb, const XForm& xf1, const XForm& xf2);
void ComputeMass(const Geom& g, double density, MassData& md);
double UpdateSweepRadius(const Geom& g, const Vec2& center);
void CollideCircles(Manifold& m, const Geom& c1, const XForm& xf1, const Geom& c2, const XForm& xf2);
void CollidePolygonAndCircle(Manifold& m, const Geom& p, const XForm& xf1, const Geom& c, const XForm& xf2);
void CollidePolygons(Manifold& m, const Geom& p1, const XForm& xf1, const Geom& p2, const XForm& xf2);
double TimeOfImpact(const Geom& g1, const Shape& s1, const Sweep& sw1, const Geom& g2, const Shape& s2,
                    const Sweep& sw2);

}  // namespace rb
