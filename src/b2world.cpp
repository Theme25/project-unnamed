// b2world.cpp - b2World / b2Body / b2BroadPhase / b2PairManager /
// b2ContactManager / b2Contact* / b2Island / b2ContactSolver, transliterated
// from the Red Ball 1 SWF. Comments flag every place where this port's
// behaviour differs from stock Box2D 2.0 C++.
#include "b2world.h"
#include <algorithm>

namespace rb {

// ------------------------------------------------------------ edge helpers
static inline ContactEdge& CEdge(World& w, int32_t ref) { return w.contacts[ref >> 1].node[ref & 1]; }
static inline JointEdge& JEdge(World& w, int32_t ref) { return w.joints[ref >> 1].node[ref & 1]; }

// ============================================================ construction

void World::Init(const GeomTable* table, const AABB& worldAABB, const Vec2& g, bool doSleep) {
    geoms = table;
    bodyList = contactList = jointList = -1;
    bodyCount = contactCount = jointCount = 0;
    positionCorrection = warmStarting = continuousPhysics = true;
    allowSleep = doSleep;
    gravity = g;
    lock = false;
    inv_dt0 = 0;
    positionIterationCount = 0;
    numBodies = numShapes = numJoints = 0;
    freeContact = -1;
    numContactSlots = 0;
    listener = PlayerContactListener();
    BP_Init(worldAABB);
    BodyDef def;
    groundBody = CreateBody(def);
}

void World::BP_Init(const AABB& worldAABB) {
    PairManager& pm = bp.pm;
    for (int i = 0; i < CAP_PAIR_TABLE; ++i) pm.hashTable[i] = NULL_PAIR;
    for (int i = 0; i < CAP_PAIRS; ++i) {
        pm.pairs[i] = Pair();
        pm.pairs[i].next = (uint32_t)(i + 1);
    }
    pm.pairs[CAP_PAIRS - 1].next = NULL_PAIR;
    pm.pairCount = 0;
    pm.pairBufferCount = 0;
    pm.freePair = 0;

    bp.worldAABB = worldAABB;
    bp.proxyCount = 0;
    for (int i = 0; i < CAP_PROXIES; ++i) bp.queryResults[i] = 0;
    for (int a = 0; a < 2; ++a)
        for (int i = 0; i < 2 * CAP_PROXIES; ++i) bp.bounds[a][i] = Bound();
    double dX = worldAABB.upperBound.x - worldAABB.lowerBound.x;
    double dY = worldAABB.upperBound.y - worldAABB.lowerBound.y;
    bp.quantizationFactor.x = settings::USHRT_MAX_ / dX;
    bp.quantizationFactor.y = settings::USHRT_MAX_ / dY;
    for (int i = 0; i < CAP_PROXIES - 1; ++i) {
        Proxy& p = bp.proxyPool[i];
        p = Proxy();
        p.SetNext((uint32_t)(i + 1));
        p.timeStamp = 0;
        p.overlapCount = BP_INVALID;
        p.userData = -1;
    }
    Proxy& last = bp.proxyPool[CAP_PROXIES - 1];
    last = Proxy();
    last.SetNext(NULL_PROXY);
    last.timeStamp = 0;
    last.overlapCount = BP_INVALID;
    last.userData = -1;
    bp.freeProxy = 0;
    bp.timeStamp = 1;
    bp.queryResultCount = 0;
}

// ============================================================ pair manager

static inline uint32_t PairHash(uint32_t proxyId1, uint32_t proxyId2) {
    uint32_t key = ((proxyId2 << 16) & 4294901760u) | proxyId1;
    key = ~key + ((key << 15) & 4294934528u);
    key ^= (key >> 12) & 1048575u;
    key += (key << 2) & 4294967292u;
    key ^= (key >> 4) & 268435455u;
    key *= 2057u;
    return key ^ ((key >> 16) & 65535u);
}

Pair* World::PM_FindHash(uint32_t id1, uint32_t id2, uint32_t hash) {
    PairManager& pm = bp.pm;
    uint32_t index = pm.hashTable[hash];
    while (index != NULL_PAIR && !(pm.pairs[index].proxyId1 == id1 && pm.pairs[index].proxyId2 == id2))
        index = pm.pairs[index].next;
    if (index == NULL_PAIR) return nullptr;
    return &pm.pairs[index];
}

Pair* World::PM_Find(uint32_t id1, uint32_t id2) {
    if (id1 > id2) std::swap(id1, id2);
    uint32_t hash = PairHash(id1, id2) & (uint32_t)(CAP_PAIR_TABLE - 1);
    return PM_FindHash(id1, id2, hash);
}

Pair* World::PM_AddPair(uint32_t id1, uint32_t id2) {
    PairManager& pm = bp.pm;
    if (id1 > id2) std::swap(id1, id2);
    uint32_t hash = PairHash(id1, id2) & (uint32_t)(CAP_PAIR_TABLE - 1);
    Pair* pair = PM_FindHash(id1, id2, hash);
    if (pair) return pair;
    uint32_t pairIndex = pm.freePair;
    if (pairIndex == NULL_PAIR) fatal("pair capacity exceeded");
    pair = &pm.pairs[pairIndex];
    pm.freePair = pair->next;
    pair->proxyId1 = id1;
    pair->proxyId2 = id2;
    pair->status = 0;
    pair->userData = PAIRDATA_NONE;
    pair->next = pm.hashTable[hash];
    pm.hashTable[hash] = pairIndex;
    ++pm.pairCount;
    return pair;
}

int32_t World::PM_RemovePair(uint32_t id1, uint32_t id2) {
    PairManager& pm = bp.pm;
    if (id1 > id2) std::swap(id1, id2);
    uint32_t hash = PairHash(id1, id2) & (uint32_t)(CAP_PAIR_TABLE - 1);
    uint32_t index = pm.hashTable[hash];
    Pair* prev = nullptr;
    while (index != NULL_PAIR) {
        Pair& pair = pm.pairs[index];
        if (pair.proxyId1 == id1 && pair.proxyId2 == id2) {
            if (prev)
                prev->next = pair.next;
            else
                pm.hashTable[hash] = pair.next;
            int32_t userData = pair.userData;
            pair.next = pm.freePair;
            pair.proxyId1 = NULL_PROXY;
            pair.proxyId2 = NULL_PROXY;
            pair.userData = PAIRDATA_NONE;
            pair.status = 0;
            pm.freePair = index;
            --pm.pairCount;
            return userData;
        }
        prev = &pair;
        index = pair.next;
    }
    return PAIRDATA_NONE;
}

void World::PM_AddBufferedPair(uint32_t id1, uint32_t id2) {
    PairManager& pm = bp.pm;
    Pair* pair = PM_AddPair(id1, id2);
    if ((pair->status & PAIR_BUFFERED) == 0) {
        pair->status |= PAIR_BUFFERED;
        if (pm.pairBufferCount >= CAP_PAIR_BUFFER) fatal("pair buffer capacity exceeded");
        pm.pairBuffer[pm.pairBufferCount].proxyId1 = pair->proxyId1;
        pm.pairBuffer[pm.pairBufferCount].proxyId2 = pair->proxyId2;
        ++pm.pairBufferCount;
    }
    pair->status &= ~PAIR_REMOVED;
}

void World::PM_RemoveBufferedPair(uint32_t id1, uint32_t id2) {
    PairManager& pm = bp.pm;
    Pair* pair = PM_Find(id1, id2);
    if (!pair) return;
    if ((pair->status & PAIR_BUFFERED) == 0) {
        pair->status |= PAIR_BUFFERED;
        if (pm.pairBufferCount >= CAP_PAIR_BUFFER) fatal("pair buffer capacity exceeded");
        pm.pairBuffer[pm.pairBufferCount].proxyId1 = pair->proxyId1;
        pm.pairBuffer[pm.pairBufferCount].proxyId2 = pair->proxyId2;
        ++pm.pairBufferCount;
    }
    pair->status |= PAIR_REMOVED;
}

// NB: unlike Box2D 2.0 C++, the AS3 port does NOT sort the pair buffer before
// committing; callbacks fire in buffer (insertion) order.
void World::PM_Commit() {
    PairManager& pm = bp.pm;
    int32_t removeCount = 0;
    for (int32_t i = 0; i < pm.pairBufferCount; ++i) {
        BufferedPair bufPair = pm.pairBuffer[i];
        Pair* pair = PM_Find(bufPair.proxyId1, bufPair.proxyId2);
        pair->status &= ~PAIR_BUFFERED;
        const Proxy& proxy1 = bp.proxyPool[pair->proxyId1];
        const Proxy& proxy2 = bp.proxyPool[pair->proxyId2];
        if (pair->status & PAIR_REMOVED) {
            if (pair->status & PAIR_FINAL) {
                (void)proxy1;
                (void)proxy2;
                PairRemoved(pair->userData);
            }
            pm.pairBuffer[removeCount].proxyId1 = pair->proxyId1;
            pm.pairBuffer[removeCount].proxyId2 = pair->proxyId2;
            ++removeCount;
        } else if ((pair->status & PAIR_FINAL) == 0) {
            pair->userData = PairAdded(proxy1.userData, proxy2.userData);
            pair->status |= PAIR_FINAL;
        }
    }
    for (int32_t i = 0; i < removeCount; ++i) PM_RemovePair(pm.pairBuffer[i].proxyId1, pm.pairBuffer[i].proxyId2);
    pm.pairBufferCount = 0;
}

// ============================================================ broadphase

static uint32_t BinarySearch(const Bound* bounds, int32_t count, uint32_t value) {
    int32_t low = 0;
    int32_t high = count - 1;
    while (low <= high) {
        int32_t mid = (low + high) / 2;  // operands are never negative here
        const Bound& b = bounds[mid];
        if (b.value > value) {
            high = mid - 1;
        } else {
            if (b.value >= value) return (uint32_t)mid;
            low = mid + 1;
        }
    }
    return (uint32_t)low;
}

void World::BP_ComputeBounds(uint32_t lowerValues[2], uint32_t upperValues[2], const AABB& aabb) const {
    const AABB& W = bp.worldAABB;
    double minVertexX = aabb.lowerBound.x;
    double minVertexY = aabb.lowerBound.y;
    minVertexX = b2Min(minVertexX, W.upperBound.x);
    minVertexY = b2Min(minVertexY, W.upperBound.y);
    minVertexX = b2Max(minVertexX, W.lowerBound.x);
    minVertexY = b2Max(minVertexY, W.lowerBound.y);
    double maxVertexX = aabb.upperBound.x;
    double maxVertexY = aabb.upperBound.y;
    maxVertexX = b2Min(maxVertexX, W.upperBound.x);
    maxVertexY = b2Min(maxVertexY, W.upperBound.y);
    maxVertexX = b2Max(maxVertexX, W.lowerBound.x);
    maxVertexY = b2Max(maxVertexY, W.lowerBound.y);
    lowerValues[0] = as3_toUint32(bp.quantizationFactor.x * (minVertexX - W.lowerBound.x)) & (65535u - 1u);
    upperValues[0] = (as3_toUint32(bp.quantizationFactor.x * (maxVertexX - W.lowerBound.x)) & 65535u) | 1u;
    lowerValues[1] = as3_toUint32(bp.quantizationFactor.y * (minVertexY - W.lowerBound.y)) & (65535u - 1u);
    upperValues[1] = (as3_toUint32(bp.quantizationFactor.y * (maxVertexY - W.lowerBound.y)) & 65535u) | 1u;
}

bool World::BP_InRange(const AABB& aabb) const {
    const AABB& W = bp.worldAABB;
    double dX = aabb.lowerBound.x;
    double dY = aabb.lowerBound.y;
    dX -= W.upperBound.x;
    dY -= W.upperBound.y;
    double d2X = W.lowerBound.x;
    double d2Y = W.lowerBound.y;
    d2X -= aabb.upperBound.x;
    d2Y -= aabb.upperBound.y;
    dX = b2Max(dX, d2X);
    dY = b2Max(dY, d2Y);
    return b2Max(dX, dY) < 0;
}

void World::BP_IncrementOverlapCount(uint32_t proxyId) {
    Proxy& proxy = bp.proxyPool[proxyId];
    if (proxy.timeStamp < bp.timeStamp) {
        proxy.timeStamp = bp.timeStamp;
        proxy.overlapCount = 1;
    } else {
        proxy.overlapCount = 2;
        if (bp.queryResultCount >= CAP_PROXIES) fatal("query result capacity exceeded");
        bp.queryResults[bp.queryResultCount] = proxyId;
        ++bp.queryResultCount;
    }
}

void World::BP_IncrementTimeStamp() {
    if (bp.timeStamp == settings::USHRT_MAX_) {
        for (int i = 0; i < CAP_PROXIES; ++i) bp.proxyPool[i].timeStamp = 0;
        bp.timeStamp = 1;
    } else {
        ++bp.timeStamp;
    }
}

void World::BP_Query(uint32_t& lowerOut, uint32_t& upperOut, uint32_t lowerValue, uint32_t upperValue,
                     Bound* bounds, uint32_t boundCount, int axis) {
    uint32_t lowerQuery = BinarySearch(bounds, (int32_t)boundCount, lowerValue);
    uint32_t upperQuery = BinarySearch(bounds, (int32_t)boundCount, upperValue);
    for (uint32_t j = lowerQuery; j < upperQuery; ++j) {
        if (bounds[j].IsLower()) BP_IncrementOverlapCount(bounds[j].proxyId);
    }
    if (lowerQuery > 0) {
        int32_t i = (int32_t)lowerQuery - 1;
        int32_t s = (int32_t)bounds[i].stabbingCount;  // uint -> int coercion as in AS3
        while (s) {
            if (i < 0) fatal("broadphase stabbing walk underflow");
            if (bounds[i].IsLower()) {
                const Proxy& proxy = bp.proxyPool[bounds[i].proxyId];
                if (lowerQuery <= proxy.upperBounds[axis]) {
                    BP_IncrementOverlapCount(bounds[i].proxyId);
                    --s;
                }
            }
            --i;
        }
    }
    lowerOut = lowerQuery;
    upperOut = upperQuery;
}

bool World::BP_TestOverlap(const uint32_t lowerValues[2], const uint32_t upperValues[2], const Proxy& p) const {
    for (int axis = 0; axis < 2; ++axis) {
        const Bound* bounds = bp.bounds[axis];
        if (lowerValues[axis] > bounds[p.upperBounds[axis]].value) return false;
        if (upperValues[axis] < bounds[p.lowerBounds[axis]].value) return false;
    }
    return true;
}

uint32_t World::BP_CreateProxy(const AABB& aabb, int32_t shape) {
    uint32_t proxyId = bp.freeProxy;
    if (proxyId == NULL_PROXY || proxyId >= (uint32_t)CAP_PROXIES) fatal("proxy capacity exceeded");
    Proxy& proxy = bp.proxyPool[proxyId];
    bp.freeProxy = proxy.GetNext();
    proxy.overlapCount = 0;
    proxy.userData = shape;
    uint32_t boundCount = 2 * (uint32_t)bp.proxyCount;
    if (boundCount + 2 > (uint32_t)(2 * CAP_PROXIES)) fatal("bound capacity exceeded");
    uint32_t lowerValues[2], upperValues[2];
    BP_ComputeBounds(lowerValues, upperValues, aabb);
    for (int axis = 0; axis < 2; ++axis) {
        Bound* bounds = bp.bounds[axis];
        uint32_t lowerIndex = 0, upperIndex = 0;
        BP_Query(lowerIndex, upperIndex, lowerValues[axis], upperValues[axis], bounds, boundCount, axis);
        std::memmove(&bounds[upperIndex + 2], &bounds[upperIndex], (boundCount - upperIndex) * sizeof(Bound));
        std::memmove(&bounds[lowerIndex + 1], &bounds[lowerIndex], (upperIndex - lowerIndex) * sizeof(Bound));
        ++upperIndex;
        bounds[lowerIndex].value = lowerValues[axis];
        bounds[lowerIndex].proxyId = proxyId;
        bounds[upperIndex].value = upperValues[axis];
        bounds[upperIndex].proxyId = proxyId;
        bounds[lowerIndex].stabbingCount = lowerIndex == 0 ? 0u : bounds[lowerIndex - 1].stabbingCount;
        bounds[upperIndex].stabbingCount = bounds[upperIndex - 1].stabbingCount;
        for (uint32_t index = lowerIndex; index < upperIndex; ++index) ++bounds[index].stabbingCount;
        for (uint32_t index = lowerIndex; index < boundCount + 2; ++index) {
            Proxy& p = bp.proxyPool[bounds[index].proxyId];
            if (bounds[index].IsLower())
                p.lowerBounds[axis] = index;
            else
                p.upperBounds[axis] = index;
        }
    }
    ++bp.proxyCount;
    for (int32_t i = 0; i < bp.queryResultCount; ++i) PM_AddBufferedPair(proxyId, bp.queryResults[i]);
    PM_Commit();
    bp.queryResultCount = 0;
    BP_IncrementTimeStamp();
    return proxyId;
}

void World::BP_DestroyProxy(uint32_t proxyId) {
    Proxy& proxy = bp.proxyPool[proxyId];
    uint32_t boundCount = 2 * (uint32_t)bp.proxyCount;
    for (int axis = 0; axis < 2; ++axis) {
        Bound* bounds = bp.bounds[axis];
        uint32_t lowerIndex = proxy.lowerBounds[axis];
        uint32_t upperIndex = proxy.upperBounds[axis];
        uint32_t lowerValue = bounds[lowerIndex].value;
        uint32_t upperValue = bounds[upperIndex].value;
        std::memmove(&bounds[lowerIndex], &bounds[lowerIndex + 1], (upperIndex - lowerIndex - 1) * sizeof(Bound));
        std::memmove(&bounds[upperIndex - 1], &bounds[upperIndex + 1], (boundCount - upperIndex - 1) * sizeof(Bound));
        for (uint32_t index = lowerIndex; index < boundCount - 2; ++index) {
            Proxy& p = bp.proxyPool[bounds[index].proxyId];
            if (bounds[index].IsLower())
                p.lowerBounds[axis] = index;
            else
                p.upperBounds[axis] = index;
        }
        for (int32_t index = (int32_t)lowerIndex; index < (int32_t)upperIndex - 1; ++index)
            --bounds[index].stabbingCount;
        uint32_t d0, d1;
        BP_Query(d0, d1, lowerValue, upperValue, bounds, boundCount - 2, axis);
    }
    for (int32_t i = 0; i < bp.queryResultCount; ++i) PM_RemoveBufferedPair(proxyId, bp.queryResults[i]);
    PM_Commit();
    bp.queryResultCount = 0;
    BP_IncrementTimeStamp();
    proxy.userData = -1;
    proxy.overlapCount = BP_INVALID;
    proxy.lowerBounds[0] = BP_INVALID;
    proxy.lowerBounds[1] = BP_INVALID;
    proxy.upperBounds[0] = BP_INVALID;
    proxy.upperBounds[1] = BP_INVALID;
    proxy.SetNext(bp.freeProxy);
    bp.freeProxy = proxyId;
    --bp.proxyCount;
}

void World::BP_MoveProxy(uint32_t proxyId, const AABB& aabb) {
    if (proxyId == NULL_PROXY || (uint32_t)settings::maxProxies <= proxyId) return;
    if (!aabb.IsValid()) return;
    uint32_t boundCount = 2 * (uint32_t)bp.proxyCount;
    Proxy& proxy = bp.proxyPool[proxyId];
    uint32_t newLower[2], newUpper[2];
    BP_ComputeBounds(newLower, newUpper, aabb);
    uint32_t oldLower[2], oldUpper[2];
    for (int axis = 0; axis < 2; ++axis) {
        oldLower[axis] = bp.bounds[axis][proxy.lowerBounds[axis]].value;
        oldUpper[axis] = bp.bounds[axis][proxy.upperBounds[axis]].value;
    }
    for (int axis = 0; axis < 2; ++axis) {
        Bound* bounds = bp.bounds[axis];
        uint32_t lowerIndex = proxy.lowerBounds[axis];
        uint32_t upperIndex = proxy.upperBounds[axis];
        uint32_t lowerValue = newLower[axis];
        uint32_t upperValue = newUpper[axis];
        int32_t deltaLower = (int32_t)lowerValue - (int32_t)bounds[lowerIndex].value;
        bounds[lowerIndex].value = lowerValue;
        int32_t deltaUpper = (int32_t)upperValue - (int32_t)bounds[upperIndex].value;
        bounds[upperIndex].value = upperValue;

        if (deltaLower < 0) {  // extend lower
            uint32_t index = lowerIndex;
            while (index > 0 && lowerValue < bounds[index - 1].value) {
                Bound& bound = bounds[index];
                Bound& prevBound = bounds[index - 1];
                uint32_t prevProxyId = prevBound.proxyId;
                Proxy& prevProxy = bp.proxyPool[prevBound.proxyId];
                ++prevBound.stabbingCount;
                if (prevBound.IsUpper()) {
                    if (BP_TestOverlap(newLower, newUpper, prevProxy)) PM_AddBufferedPair(proxyId, prevProxyId);
                    ++prevProxy.upperBounds[axis];
                    ++bound.stabbingCount;
                } else {
                    ++prevProxy.lowerBounds[axis];
                    --bound.stabbingCount;
                }
                --proxy.lowerBounds[axis];
                std::swap(bound, prevBound);
                --index;
            }
        }
        if (deltaUpper > 0) {  // extend upper
            uint32_t index = upperIndex;
            while (index < boundCount - 1 && bounds[index + 1].value <= upperValue) {
                Bound& bound = bounds[index];
                Bound& nextBound = bounds[index + 1];
                uint32_t nextProxyId = nextBound.proxyId;
                Proxy& nextProxy = bp.proxyPool[nextProxyId];
                ++nextBound.stabbingCount;
                if (nextBound.IsLower()) {
                    if (BP_TestOverlap(newLower, newUpper, nextProxy)) PM_AddBufferedPair(proxyId, nextProxyId);
                    --nextProxy.lowerBounds[axis];
                    ++bound.stabbingCount;
                } else {
                    --nextProxy.upperBounds[axis];
                    --bound.stabbingCount;
                }
                ++proxy.upperBounds[axis];
                std::swap(bound, nextBound);
                ++index;
            }
        }
        if (deltaLower > 0) {  // shrink lower
            uint32_t index = lowerIndex;
            while (index < boundCount - 1 && bounds[index + 1].value <= lowerValue) {
                Bound& bound = bounds[index];
                Bound& nextBound = bounds[index + 1];
                uint32_t nextProxyId = nextBound.proxyId;
                Proxy& nextProxy = bp.proxyPool[nextProxyId];
                --nextBound.stabbingCount;
                if (nextBound.IsUpper()) {
                    if (BP_TestOverlap(oldLower, oldUpper, nextProxy)) PM_RemoveBufferedPair(proxyId, nextProxyId);
                    --nextProxy.upperBounds[axis];
                    --bound.stabbingCount;
                } else {
                    --nextProxy.lowerBounds[axis];
                    ++bound.stabbingCount;
                }
                ++proxy.lowerBounds[axis];
                std::swap(bound, nextBound);
                ++index;
            }
        }
        if (deltaUpper < 0) {  // shrink upper
            uint32_t index = upperIndex;
            while (index > 0 && upperValue < bounds[index - 1].value) {
                Bound& bound = bounds[index];
                Bound& prevBound = bounds[index - 1];
                uint32_t prevProxyId = prevBound.proxyId;
                Proxy& prevProxy = bp.proxyPool[prevProxyId];
                --prevBound.stabbingCount;
                if (prevBound.IsLower()) {
                    if (BP_TestOverlap(oldLower, oldUpper, prevProxy)) PM_RemoveBufferedPair(proxyId, prevProxyId);
                    ++prevProxy.lowerBounds[axis];
                    --bound.stabbingCount;
                } else {
                    ++prevProxy.upperBounds[axis];
                    ++bound.stabbingCount;
                }
                --proxy.upperBounds[axis];
                std::swap(bound, prevBound);
                --index;
            }
        }
    }
}

int32_t World::BP_QueryAABB(const AABB& aabb, int32_t* out, int32_t maxCount) {
    uint32_t lowerValues[2], upperValues[2];
    BP_ComputeBounds(lowerValues, upperValues, aabb);
    uint32_t lowerIndex = 0, upperIndex = 0;
    BP_Query(lowerIndex, upperIndex, lowerValues[0], upperValues[0], bp.bounds[0], 2 * (uint32_t)bp.proxyCount, 0);
    BP_Query(lowerIndex, upperIndex, lowerValues[1], upperValues[1], bp.bounds[1], 2 * (uint32_t)bp.proxyCount, 1);
    int32_t count = 0;
    for (int32_t i = 0; i < bp.queryResultCount && count < maxCount; ++i, ++count)
        out[i] = bp.proxyPool[bp.queryResults[i]].userData;
    bp.queryResultCount = 0;
    BP_IncrementTimeStamp();
    return count;
}

void World::BP_Commit() { PM_Commit(); }

// ============================================================ shapes <-> broadphase

void World::Shape_CreateProxy(int32_t s, const XForm& xf) {
    AABB aabb;
    ComputeAABB((*geoms)[shapes[s].geom], aabb, xf);
    if (BP_InRange(aabb))
        shapes[s].proxyId = BP_CreateProxy(aabb, s);
    else
        shapes[s].proxyId = NULL_PROXY;
}

void World::Shape_DestroyProxy(int32_t s) {
    if (shapes[s].proxyId != NULL_PROXY) {
        BP_DestroyProxy(shapes[s].proxyId);
        shapes[s].proxyId = NULL_PROXY;
    }
}

bool World::Shape_Synchronize(int32_t s, const XForm& xf1, const XForm& xf2) {
    if (shapes[s].proxyId == NULL_PROXY) return false;
    AABB aabb;
    ComputeSweptAABB((*geoms)[shapes[s].geom], aabb, xf1, xf2);
    if (BP_InRange(aabb)) {
        BP_MoveProxy(shapes[s].proxyId, aabb);
        return true;
    }
    return false;
}

void World::Shape_RefilterProxy(int32_t s, const XForm& xf) {
    if (shapes[s].proxyId == NULL_PROXY) return;
    BP_DestroyProxy(shapes[s].proxyId);
    AABB aabb;
    ComputeAABB((*geoms)[shapes[s].geom], aabb, xf);
    if (BP_InRange(aabb))
        shapes[s].proxyId = BP_CreateProxy(aabb, s);
    else
        shapes[s].proxyId = NULL_PROXY;
}

void World::Refilter(int32_t s) { Shape_RefilterProxy(s, bodies[shapes[s].body].xf); }

// ============================================================ listener emulation

void World::ListenerAdd(int32_t s1, int32_t s2) {
    int32_t b1 = shapes[s1].body, b2 = shapes[s2].body;
    PlayerContactListener& L = listener;
    if (b1 == L.playerBody) {
        if (L.count >= CAP_PLAYER_CONTACTS) fatal("player contact list overflow");
        L.bodies[L.count++] = b2;
    }
    if (b2 == L.playerBody) {
        if (L.count >= CAP_PLAYER_CONTACTS) fatal("player contact list overflow");
        L.bodies[L.count++] = b1;
    }
}

static bool ListContains(const PlayerContactListener& L, int32_t b) {
    for (int32_t i = 0; i < L.count; ++i)
        if (L.bodies[i] == b) return true;
    return false;
}

static void ListSpliceFirst(PlayerContactListener& L) {  // Array.splice(<object>=>0, 1)
    if (L.count == 0) return;
    for (int32_t i = 1; i < L.count; ++i) L.bodies[i - 1] = L.bodies[i];
    --L.count;
}

void World::ListenerRemove(int32_t s1, int32_t s2) {
    int32_t b1 = shapes[s1].body, b2 = shapes[s2].body;
    PlayerContactListener& L = listener;
    if (b1 == L.playerBody && ListContains(L, b2)) ListSpliceFirst(L);
    if (b2 == L.playerBody && ListContains(L, b1)) ListSpliceFirst(L);
}

// ============================================================ contacts

int32_t World::AllocContact() {
    int32_t c;
    if (freeContact != -1) {
        c = freeContact;
        freeContact = contacts[c].nextFree;
    } else {
        if (numContactSlots >= CAP_CONTACTS) fatal("contact capacity exceeded");
        c = numContactSlots++;
    }
    contacts[c] = Contact();
    return c;
}

static bool ShouldCollide(const Shape& s1, const Shape& s2) {
    const FilterData& f1 = s1.filter;
    const FilterData& f2 = s2.filter;
    if (f1.groupIndex == f2.groupIndex && f1.groupIndex != 0) return f1.groupIndex > 0;
    return (f1.maskBits & f2.categoryBits) != 0 && (f1.categoryBits & f2.maskBits) != 0;
}

// b2Contact.Create + concrete constructors
int32_t World::CreateContact(int32_t sA, int32_t sB) {
    int32_t t1 = shapes[sA].type, t2 = shapes[sB].type;
    int32_t kind;
    int32_t s1 = sA, s2 = sB;
    if (t1 == e_circleShape && t2 == e_circleShape)
        kind = CK_CIRCLE;
    else if (t1 == e_polygonShape && t2 == e_circleShape)
        kind = CK_POLYCIRCLE;
    else if (t1 == e_circleShape && t2 == e_polygonShape) {
        kind = CK_POLYCIRCLE;  // non-primary register: swapped; the normal-negation loop runs 0 times
        s1 = sB;
        s2 = sA;
    } else if (t1 == e_polygonShape && t2 == e_polygonShape)
        kind = CK_POLYGON;
    else
        return -1;
    int32_t c = AllocContact();
    Contact& ct = contacts[c];
    ct.kind = kind;
    ct.flags = 0;
    if (shapes[s1].isSensor || shapes[s2].isSensor) ct.flags |= CF_NONSOLID;
    ct.shape1 = s1;
    ct.shape2 = s2;
    ct.manifoldCount = 0;
    ct.friction = as3_sqrt(shapes[s1].friction * shapes[s2].friction);
    ct.restitution = b2Max(shapes[s1].restitution, shapes[s2].restitution);
    ct.prev = ct.next = -1;
    ct.node[0] = ContactEdge();
    ct.node[1] = ContactEdge();
    ct.manifold.pointCount = 0;
    if (kind == CK_CIRCLE || kind == CK_POLYCIRCLE) {
        ct.manifold.points[0].normalImpulse = 0;
        ct.manifold.points[0].tangentImpulse = 0;
    }
    return c;
}

int32_t World::PairAdded(int32_t sA, int32_t sB) {
    int32_t bA = shapes[sA].body, bB = shapes[sB].body;
    if (bodies[bA].IsStatic() && bodies[bB].IsStatic()) return PAIRDATA_NULLCONTACT;
    if (bA == bB) return PAIRDATA_NULLCONTACT;
    if (IsConnected(bB, bA)) return PAIRDATA_NULLCONTACT;
    if (!ShouldCollide(shapes[sA], shapes[sB])) return PAIRDATA_NULLCONTACT;
    int32_t c = CreateContact(sA, sB);
    if (c == -1) return PAIRDATA_NULLCONTACT;
    Contact& ct = contacts[c];
    int32_t b1 = shapes[ct.shape1].body, b2 = shapes[ct.shape2].body;
    ct.prev = -1;
    ct.next = contactList;
    if (contactList != -1) contacts[contactList].prev = c;
    contactList = c;
    const int32_t e1 = c * 2 + 0, e2 = c * 2 + 1;
    ct.node[0].other = b2;
    ct.node[0].prev = -1;
    ct.node[0].next = bodies[b1].contactList;
    if (bodies[b1].contactList != -1) CEdge(*this, bodies[b1].contactList).prev = e1;
    bodies[b1].contactList = e1;
    ct.node[1].other = b1;
    ct.node[1].prev = -1;
    ct.node[1].next = bodies[b2].contactList;
    if (bodies[b2].contactList != -1) CEdge(*this, bodies[b2].contactList).prev = e2;
    bodies[b2].contactList = e2;
    ++contactCount;
    return c;
}

void World::PairRemoved(int32_t userData) {
    if (userData == PAIRDATA_NONE) return;
    if (userData == PAIRDATA_NULLCONTACT) return;
    DestroyContact(userData);
}

void World::DestroyContact(int32_t c) {
    Contact& ct = contacts[c];
    if (ct.manifoldCount > 0 && listener.enabled) {
        for (int32_t i = 0; i < ct.manifoldCount; ++i)
            for (int32_t j = 0; j < ct.manifold.pointCount; ++j) ListenerRemove(ct.shape1, ct.shape2);
    }
    if (ct.prev != -1) contacts[ct.prev].next = ct.next;
    if (ct.next != -1) contacts[ct.next].prev = ct.prev;
    if (c == contactList) contactList = ct.next;
    int32_t b1 = shapes[ct.shape1].body, b2 = shapes[ct.shape2].body;
    const int32_t e1 = c * 2 + 0, e2 = c * 2 + 1;
    if (ct.node[0].prev != -1) CEdge(*this, ct.node[0].prev).next = ct.node[0].next;
    if (ct.node[0].next != -1) CEdge(*this, ct.node[0].next).prev = ct.node[0].prev;
    if (e1 == bodies[b1].contactList) bodies[b1].contactList = ct.node[0].next;
    if (ct.node[1].prev != -1) CEdge(*this, ct.node[1].prev).next = ct.node[1].next;
    if (ct.node[1].next != -1) CEdge(*this, ct.node[1].next).prev = ct.node[1].prev;
    if (e2 == bodies[b2].contactList) bodies[b2].contactList = ct.node[1].next;
    // b2Contact.Destroy
    if (ct.manifoldCount > 0) {
        bodies[b1].WakeUp();
        bodies[b2].WakeUp();
    }
    --contactCount;
    ct.kind = CK_NONE;
    ct.nextFree = freeContact;
    freeContact = c;
}

void World::ContactEvaluate(int32_t c) {
    Contact& ct = contacts[c];
    const Body& body1 = bodies[shapes[ct.shape1].body];
    const Body& body2 = bodies[shapes[ct.shape2].body];
    const Geom& g1 = (*geoms)[shapes[ct.shape1].geom];
    const Geom& g2 = (*geoms)[shapes[ct.shape2].geom];
    const Manifold m0 = ct.manifold;
    Manifold& m = ct.manifold;
    const bool hasListener = listener.enabled;
    if (ct.kind == CK_CIRCLE) {
        CollideCircles(m, g1, body1.xf, g2, body2.xf);
        if (m.pointCount > 0) {
            ct.manifoldCount = 1;
            ManifoldPoint& mp = m.points[0];
            if (m0.pointCount == 0) {
                mp.normalImpulse = 0;
                mp.tangentImpulse = 0;
                if (hasListener) ListenerAdd(ct.shape1, ct.shape2);
            } else {
                mp.normalImpulse = m0.points[0].normalImpulse;
                mp.tangentImpulse = m0.points[0].tangentImpulse;
                // listener.Persist: no-op in MyContactListener
            }
        } else {
            ct.manifoldCount = 0;
            if (m0.pointCount > 0 && hasListener) ListenerRemove(ct.shape1, ct.shape2);
        }
        return;
    }
    if (ct.kind == CK_POLYCIRCLE)
        CollidePolygonAndCircle(m, g1, body1.xf, g2, body2.xf);
    else
        CollidePolygons(m, g1, body1.xf, g2, body2.xf);
    bool persisted[2] = {false, false};
    if (m.pointCount > 0) {
        for (int32_t i = 0; i < m.pointCount; ++i) {
            ManifoldPoint& mp = m.points[i];
            mp.normalImpulse = 0;
            mp.tangentImpulse = 0;
            bool found = false;
            uint32_t key = mp.key;
            for (int32_t j = 0; j < m0.pointCount; ++j) {
                if (persisted[j] != true) {
                    const ManifoldPoint& mp0 = m0.points[j];
                    if (mp0.key == key) {
                        persisted[j] = true;
                        mp.normalImpulse = mp0.normalImpulse;
                        mp.tangentImpulse = mp0.tangentImpulse;
                        found = true;
                        break;  // listener.Persist: no-op
                    }
                }
            }
            if (found == false && hasListener) ListenerAdd(ct.shape1, ct.shape2);
        }
        ct.manifoldCount = 1;
    } else {
        ct.manifoldCount = 0;
    }
    if (!hasListener) return;
    for (int32_t i = 0; i < m0.pointCount; ++i)
        if (!persisted[i]) ListenerRemove(ct.shape1, ct.shape2);
}

void World::ContactUpdate(int32_t c) {
    int32_t oldCount = contacts[c].manifoldCount;
    ContactEvaluate(c);
    Contact& ct = contacts[c];
    int32_t newCount = ct.manifoldCount;
    Body& body1 = bodies[shapes[ct.shape1].body];
    Body& body2 = bodies[shapes[ct.shape2].body];
    if (newCount == 0 && oldCount > 0) {
        body1.WakeUp();
        body2.WakeUp();
    }
    if (body1.IsStatic() || body1.IsBullet() || body2.IsStatic() || body2.IsBullet())
        ct.flags &= ~CF_SLOW;
    else
        ct.flags |= CF_SLOW;
}

void World::CollideAll() {
    for (int32_t c = contactList; c != -1; c = contacts[c].next) {
        const Body& b1 = bodies[shapes[contacts[c].shape1].body];
        const Body& b2 = bodies[shapes[contacts[c].shape2].body];
        if (!(b1.IsSleeping() && b2.IsSleeping())) ContactUpdate(c);
    }
}

// ============================================================ bodies

int32_t World::CreateBody(const BodyDef& def) {
    if (lock) return -1;
    if (numBodies >= CAP_BODIES) fatal("body capacity exceeded");
    int32_t b = numBodies++;
    Body& body = bodies[b];
    body = Body();
    body.flags = 0;
    if (def.isBullet) body.flags |= BF_BULLET;
    if (def.fixedRotation) body.flags |= BF_FIXEDROTATION;
    if (def.allowSleep) body.flags |= BF_ALLOWSLEEP;
    if (def.isSleeping) body.flags |= BF_SLEEP;
    body.xf.position.SetV(def.position);
    body.xf.R.Set(def.angle);
    body.sweep.localCenter.SetV(def.massData.center);
    body.sweep.t0 = 1;
    body.sweep.a0 = body.sweep.a = def.angle;
    const Mat22& R = body.xf.R;
    const Vec2& lc = body.sweep.localCenter;
    body.sweep.c.x = R.col1.x * lc.x + R.col2.x * lc.y;
    body.sweep.c.y = R.col1.y * lc.x + R.col2.y * lc.y;
    body.sweep.c.x += body.xf.position.x;
    body.sweep.c.y += body.xf.position.y;
    body.sweep.c0.SetV(body.sweep.c);
    body.jointList = -1;
    body.contactList = -1;
    body.linearDamping = def.linearDamping;
    body.angularDamping = def.angularDamping;
    body.force.Set(0, 0);
    body.torque = 0;
    body.linearVelocity.SetZero();
    body.angularVelocity = 0;
    body.sleepTime = 0;
    body.invMass = 0;
    body.I = 0;
    body.invI = 0;
    body.mass = def.massData.mass;
    if (body.mass > 0) body.invMass = 1 / body.mass;
    if ((body.flags & BF_FIXEDROTATION) == 0) body.I = def.massData.I;
    if (body.I > 0) body.invI = 1 / body.I;
    body.type = (body.invMass == 0 && body.invI == 0) ? BT_STATIC : BT_DYNAMIC;
    body.userTag = def.userTag;
    body.shapeList = -1;
    body.shapeCount = 0;
    body.inWorld = true;
    body.prev = -1;
    body.next = bodyList;
    if (bodyList != -1) bodies[bodyList].prev = b;
    bodyList = b;
    ++bodyCount;
    return b;
}

void World::DestroyBody(int32_t b) {
    if (lock) return;
    Body& body = bodies[b];
    int32_t je = body.jointList;
    while (je != -1) {
        (void)JEdge;
        fatal("DestroyBody with joints: joints not implemented yet");
    }
    int32_t s = body.shapeList;
    while (s != -1) {
        int32_t s0 = s;
        s = shapes[s].next;
        Shape_DestroyProxy(s0);
    }
    if (body.prev != -1) bodies[body.prev].next = body.next;
    if (body.next != -1) bodies[body.next].prev = body.prev;
    if (b == bodyList) bodyList = body.next;
    --bodyCount;
    body.inWorld = false;
}

int32_t World::CreateShape(int32_t b, int32_t geom, const ShapeDef& def) {
    if (lock) return -1;
    if (numShapes >= CAP_SHAPES) fatal("shape capacity exceeded");
    int32_t s = numShapes++;
    Shape& sh = shapes[s];
    sh = Shape();
    sh.geom = geom;
    sh.type = (*geoms)[geom].type;
    sh.friction = def.friction;
    sh.restitution = def.restitution;
    sh.density = def.density;
    sh.body = -1;
    sh.sweepRadius = 0;
    sh.next = -1;
    sh.proxyId = NULL_PROXY;
    sh.filter = def.filter;
    sh.isSensor = def.isSensor;
    Body& body = bodies[b];
    sh.next = body.shapeList;
    body.shapeList = s;
    ++body.shapeCount;
    sh.body = b;
    Shape_CreateProxy(s, body.xf);
    shapes[s].sweepRadius = UpdateSweepRadius((*geoms)[geom], bodies[b].sweep.localCenter);
    return s;
}

void World::SetMassFromShapes(int32_t b) {
    if (lock) return;
    Body& body = bodies[b];
    body.mass = 0;
    body.invMass = 0;
    body.I = 0;
    body.invI = 0;
    double centerX = 0, centerY = 0;
    MassData md;
    for (int32_t s = body.shapeList; s != -1; s = shapes[s].next) {
        ComputeMass((*geoms)[shapes[s].geom], shapes[s].density, md);
        body.mass += md.mass;
        centerX += md.mass * md.center.x;
        centerY += md.mass * md.center.y;
        body.I += md.I;
    }
    if (body.mass > 0) {
        body.invMass = 1 / body.mass;
        centerX *= body.invMass;
        centerY *= body.invMass;
    }
    if (body.I > 0 && (body.flags & BF_FIXEDROTATION) == 0) {
        body.I -= body.mass * (centerX * centerX + centerY * centerY);
        body.invI = 1 / body.I;
    } else {
        body.I = 0;
        body.invI = 0;
    }
    body.sweep.localCenter.Set(centerX, centerY);
    const Mat22& R = body.xf.R;
    const Vec2& lc = body.sweep.localCenter;
    body.sweep.c.x = R.col1.x * lc.x + R.col2.x * lc.y;
    body.sweep.c.y = R.col1.y * lc.x + R.col2.y * lc.y;
    body.sweep.c.x += body.xf.position.x;
    body.sweep.c.y += body.xf.position.y;
    body.sweep.c0.SetV(body.sweep.c);
    for (int32_t s = body.shapeList; s != -1; s = shapes[s].next)
        shapes[s].sweepRadius = UpdateSweepRadius((*geoms)[shapes[s].geom], body.sweep.localCenter);
    int32_t oldType = body.type;
    body.type = (body.invMass == 0 && body.invI == 0) ? BT_STATIC : BT_DYNAMIC;
    // Bodies are created static (no mass yet) so their proxies were paired as
    // static; the type flip re-creates every proxy (new ids, new pairs).
    if (oldType != body.type)
        for (int32_t s = body.shapeList; s != -1; s = shapes[s].next) Shape_RefilterProxy(s, bodies[b].xf);
}

void World::ApplyForce(int32_t b, const Vec2& force, const Vec2& point) {
    Body& body = bodies[b];
    if (body.IsSleeping()) body.WakeUp();
    body.force.x += force.x;
    body.force.y += force.y;
    body.torque += (point.x - body.sweep.c.x) * force.y - (point.y - body.sweep.c.y) * force.x;
}

void World::ApplyImpulse(int32_t b, const Vec2& impulse, const Vec2& point) {
    Body& body = bodies[b];
    if (body.IsSleeping()) body.WakeUp();
    body.linearVelocity.x += body.invMass * impulse.x;
    body.linearVelocity.y += body.invMass * impulse.y;
    body.angularVelocity +=
        body.invI * ((point.x - body.sweep.c.x) * impulse.y - (point.y - body.sweep.c.y) * impulse.x);
}

bool World::IsConnected(int32_t b, int32_t other) const {
    for (int32_t je = bodies[b].jointList; je != -1;) {
        const JointEdge& e = joints[je >> 1].node[je & 1];
        if (e.other == other) return joints[je >> 1].collideConnected == false;
        je = e.next;
    }
    return false;
}

void World::SynchronizeTransform(int32_t b) {
    Body& body = bodies[b];
    body.xf.R.Set(body.sweep.a);
    const Mat22& R = body.xf.R;
    const Vec2& lc = body.sweep.localCenter;
    body.xf.position.x = body.sweep.c.x - (R.col1.x * lc.x + R.col2.x * lc.y);
    body.xf.position.y = body.sweep.c.y - (R.col1.y * lc.x + R.col2.y * lc.y);
}

bool World::SynchronizeShapes(int32_t b) {
    Body& body = bodies[b];
    XForm xf1;
    xf1.R.Set(body.sweep.a0);
    const Mat22& R = xf1.R;
    const Vec2& lc = body.sweep.localCenter;
    xf1.position.x = body.sweep.c0.x - (R.col1.x * lc.x + R.col2.x * lc.y);
    xf1.position.y = body.sweep.c0.y - (R.col1.y * lc.x + R.col2.y * lc.y);
    bool inRange = true;
    for (int32_t s = body.shapeList; s != -1; s = shapes[s].next) {
        inRange = Shape_Synchronize(s, xf1, bodies[b].xf);
        if (inRange == false) break;
    }
    if (inRange == false) {
        Body& bb = bodies[b];
        bb.flags |= BF_FROZEN;
        bb.linearVelocity.SetZero();
        bb.angularVelocity = 0;
        for (int32_t s = bb.shapeList; s != -1; s = shapes[s].next) Shape_DestroyProxy(s);
        return false;
    }
    return true;
}

void World::Advance(int32_t b, double t) {
    Body& body = bodies[b];
    body.sweep.Advance(t);
    body.sweep.c.SetV(body.sweep.c0);
    body.sweep.a = body.sweep.a0;
    SynchronizeTransform(b);
}

int32_t World::Query(const AABB& aabb, int32_t* out, int32_t maxCount) {
    return BP_QueryAABB(aabb, out, maxCount);
}

// ============================================================ contact solver

namespace {

struct ContactConstraintPoint {
    Vec2 localAnchor1, localAnchor2, r1, r2;
    double normalImpulse, tangentImpulse, positionImpulse;
    double normalMass, tangentMass, equalizedMass;
    double separation, velocityBias;
};

struct ContactConstraint {
    ContactConstraintPoint points[settings::maxManifoldPoints];
    Vec2 normal;
    int32_t body1, body2;
    int32_t contact;  // owner of the manifold
    double friction, restitution;
    int32_t pointCount;
};

struct ContactSolver {
    World& w;
    TimeStep step;
    std::vector<ContactConstraint>& constraints;
    int32_t constraintCount = 0;

    ContactSolver(World& world, const TimeStep& s, const int32_t* contactIdx, int32_t contactCount,
                  std::vector<ContactConstraint>& storage)
        : w(world), constraints(storage) {
        step.dt = s.dt;
        step.inv_dt = s.inv_dt;
        step.maxIterations = s.maxIterations;
        for (int32_t i = 0; i < contactCount; ++i) constraintCount += w.contacts[contactIdx[i]].manifoldCount;
        if ((int32_t)constraints.size() < constraintCount) constraints.resize((size_t)constraintCount);
        int32_t count = 0;
        for (int32_t i = 0; i < contactCount; ++i) {
            const Contact& contact = w.contacts[contactIdx[i]];
            int32_t b1i = w.shapes[contact.shape1].body;
            int32_t b2i = w.shapes[contact.shape2].body;
            const Body& b1 = w.bodies[b1i];
            const Body& b2 = w.bodies[b2i];
            int32_t manifoldCount = contact.manifoldCount;
            double friction = contact.friction;
            double restitution = contact.restitution;
            double v1X = b1.linearVelocity.x;
            double v1Y = b1.linearVelocity.y;
            double v2X = b2.linearVelocity.x;
            double v2Y = b2.linearVelocity.y;
            double w1 = b1.angularVelocity;
            double w2 = b2.angularVelocity;
            for (int32_t j = 0; j < manifoldCount; ++j) {
                const Manifold& manifold = contact.manifold;
                double normalX = manifold.normal.x;
                double normalY = manifold.normal.y;
                ContactConstraint& cc = constraints[(size_t)count];
                cc.body1 = b1i;
                cc.body2 = b2i;
                cc.contact = contactIdx[i];
                cc.normal.x = normalX;
                cc.normal.y = normalY;
                cc.pointCount = manifold.pointCount;
                cc.friction = friction;
                cc.restitution = restitution;
                for (int32_t k = 0; k < cc.pointCount; ++k) {
                    const ManifoldPoint& cp = manifold.points[k];
                    ContactConstraintPoint& ccp = cc.points[k];
                    ccp.normalImpulse = cp.normalImpulse;
                    ccp.tangentImpulse = cp.tangentImpulse;
                    ccp.separation = cp.separation;
                    ccp.positionImpulse = 0;
                    ccp.localAnchor1.SetV(cp.localPoint1);
                    ccp.localAnchor2.SetV(cp.localPoint2);
                    const Mat22* R = &b1.xf.R;
                    double r1X = cp.localPoint1.x - b1.sweep.localCenter.x;
                    double r1Y = cp.localPoint1.y - b1.sweep.localCenter.y;
                    double tX = R->col1.x * r1X + R->col2.x * r1Y;
                    r1Y = R->col1.y * r1X + R->col2.y * r1Y;
                    r1X = tX;
                    ccp.r1.Set(r1X, r1Y);
                    R = &b2.xf.R;
                    double r2X = cp.localPoint2.x - b2.sweep.localCenter.x;
                    double r2Y = cp.localPoint2.y - b2.sweep.localCenter.y;
                    tX = R->col1.x * r2X + R->col2.x * r2Y;
                    r2Y = R->col1.y * r2X + R->col2.y * r2Y;
                    r2X = tX;
                    ccp.r2.Set(r2X, r2Y);
                    double r1Sqr = r1X * r1X + r1Y * r1Y;
                    double r2Sqr = r2X * r2X + r2Y * r2Y;
                    double rn1 = r1X * normalX + r1Y * normalY;
                    double rn2 = r2X * normalX + r2Y * normalY;
                    double kNormal = b1.invMass + b2.invMass;
                    kNormal = kNormal + (b1.invI * (r1Sqr - rn1 * rn1) + b2.invI * (r2Sqr - rn2 * rn2));
                    ccp.normalMass = 1 / kNormal;
                    double kEqualized = b1.mass * b1.invMass + b2.mass * b2.invMass;
                    kEqualized = kEqualized + (b1.mass * b1.invI * (r1Sqr - rn1 * rn1) +
                                               b2.mass * b2.invI * (r2Sqr - rn2 * rn2));
                    ccp.equalizedMass = 1 / kEqualized;
                    double tangentX = normalY;
                    double tangentY = -normalX;
                    double rt1 = r1X * tangentX + r1Y * tangentY;
                    double rt2 = r2X * tangentX + r2Y * tangentY;
                    double kTangent = b1.invMass + b2.invMass;
                    kTangent = kTangent + (b1.invI * (r1Sqr - rt1 * rt1) + b2.invI * (r2Sqr - rt2 * rt2));
                    ccp.tangentMass = 1 / kTangent;
                    // NB: hard-coded 60 (not inv_dt) in this port
                    ccp.velocityBias = 0;
                    if (ccp.separation > 0) ccp.velocityBias = -60 * ccp.separation;
                    double vRelX = v2X + -w2 * r2Y - v1X - -w1 * r1Y;
                    double vRelY = v2Y + w2 * r2X - v1Y - w1 * r1X;
                    double vRel = cc.normal.x * vRelX + cc.normal.y * vRelY;
                    if (vRel < -settings::velocityThreshold) ccp.velocityBias += -cc.restitution * vRel;
                }
                ++count;
            }
        }
    }

    void InitVelocityConstraints(const TimeStep& s) {
        for (int32_t i = 0; i < constraintCount; ++i) {
            ContactConstraint& c = constraints[(size_t)i];
            Body& b1 = w.bodies[c.body1];
            Body& b2 = w.bodies[c.body2];
            double invMass1 = b1.invMass, invI1 = b1.invI;
            double invMass2 = b2.invMass, invI2 = b2.invI;
            double normalX = c.normal.x;
            double normalY = c.normal.y;
            double tangentX = normalY;
            double tangentY = -normalX;
            if (s.warmStarting) {
                int32_t tCount = c.pointCount;
                for (int32_t j = 0; j < tCount; ++j) {
                    ContactConstraintPoint& ccp = c.points[j];
                    ccp.normalImpulse *= s.dtRatio;
                    ccp.tangentImpulse *= s.dtRatio;
                    double PX = ccp.normalImpulse * normalX + ccp.tangentImpulse * tangentX;
                    double PY = ccp.normalImpulse * normalY + ccp.tangentImpulse * tangentY;
                    b1.angularVelocity -= invI1 * (ccp.r1.x * PY - ccp.r1.y * PX);
                    b1.linearVelocity.x -= invMass1 * PX;
                    b1.linearVelocity.y -= invMass1 * PY;
                    b2.angularVelocity += invI2 * (ccp.r2.x * PY - ccp.r2.y * PX);
                    b2.linearVelocity.x += invMass2 * PX;
                    b2.linearVelocity.y += invMass2 * PY;
                }
            } else {
                for (int32_t j = 0; j < c.pointCount; ++j) {
                    c.points[j].normalImpulse = 0;
                    c.points[j].tangentImpulse = 0;
                }
            }
        }
    }

    void SolveVelocityConstraints() {
        for (int32_t i = 0; i < constraintCount; ++i) {
            ContactConstraint& c = constraints[(size_t)i];
            Body& b1 = w.bodies[c.body1];
            Body& b2 = w.bodies[c.body2];
            double w1 = b1.angularVelocity;
            double w2 = b2.angularVelocity;
            Vec2& v1 = b1.linearVelocity;
            Vec2& v2 = b2.linearVelocity;
            double invMass1 = b1.invMass, invI1 = b1.invI;
            double invMass2 = b2.invMass, invI2 = b2.invI;
            double normalX = c.normal.x;
            double normalY = c.normal.y;
            double tangentX = normalY;
            double tangentY = -normalX;
            double friction = c.friction;
            int32_t tCount = c.pointCount;
            for (int32_t j = 0; j < tCount; ++j) {
                ContactConstraintPoint& ccp = c.points[j];
                double dvX = v2.x + -w2 * ccp.r2.y - v1.x - -w1 * ccp.r1.y;
                double dvY = v2.y + w2 * ccp.r2.x - v1.y - w1 * ccp.r1.x;
                double vn = dvX * normalX + dvY * normalY;
                double lambda_n = -ccp.normalMass * (vn - ccp.velocityBias);
                double vt = dvX * tangentX + dvY * tangentY;
                double lambda_t = ccp.tangentMass * -vt;
                double newImpulse_n = b2Max(ccp.normalImpulse + lambda_n, 0);
                lambda_n = newImpulse_n - ccp.normalImpulse;
                // NB: friction bound uses the normal impulse from BEFORE this
                // iteration's update (AS3 port quirk).
                double maxFriction = friction * ccp.normalImpulse;
                double newImpulse_t = b2Clamp(ccp.tangentImpulse + lambda_t, -maxFriction, maxFriction);
                lambda_t = newImpulse_t - ccp.tangentImpulse;
                double PX = lambda_n * normalX + lambda_t * tangentX;
                double PY = lambda_n * normalY + lambda_t * tangentY;
                v1.x -= invMass1 * PX;
                v1.y -= invMass1 * PY;
                w1 -= invI1 * (ccp.r1.x * PY - ccp.r1.y * PX);
                v2.x += invMass2 * PX;
                v2.y += invMass2 * PY;
                w2 += invI2 * (ccp.r2.x * PY - ccp.r2.y * PX);
                ccp.normalImpulse = newImpulse_n;
                ccp.tangentImpulse = newImpulse_t;
            }
            b1.angularVelocity = w1;
            b2.angularVelocity = w2;
        }
    }

    void FinalizeVelocityConstraints() {
        for (int32_t i = 0; i < constraintCount; ++i) {
            ContactConstraint& c = constraints[(size_t)i];
            Manifold& m = w.contacts[c.contact].manifold;
            for (int32_t j = 0; j < c.pointCount; ++j) {
                m.points[j].normalImpulse = c.points[j].normalImpulse;
                m.points[j].tangentImpulse = c.points[j].tangentImpulse;
            }
        }
    }

    bool SolvePositionConstraints(double baumgarte) {
        double minSeparation = 0;
        for (int32_t i = 0; i < constraintCount; ++i) {
            ContactConstraint& c = constraints[(size_t)i];
            int32_t b1i = c.body1, b2i = c.body2;
            Body& b1 = w.bodies[b1i];
            Body& b2 = w.bodies[b2i];
            Vec2& b1_c = b1.sweep.c;
            double b1_a = b1.sweep.a;
            Vec2& b2_c = b2.sweep.c;
            double b2_a = b2.sweep.a;
            double invMass1 = b1.mass * b1.invMass;
            double invI1 = b1.mass * b1.invI;
            double invMass2 = b2.mass * b2.invMass;
            double invI2 = b2.mass * b2.invI;
            double normalX = c.normal.x;
            double normalY = c.normal.y;
            int32_t tCount = c.pointCount;
            for (int32_t j = 0; j < tCount; ++j) {
                ContactConstraintPoint& ccp = c.points[j];
                const Mat22* R = &b1.xf.R;
                const Vec2* lc = &b1.sweep.localCenter;
                double r1X = ccp.localAnchor1.x - lc->x;
                double r1Y = ccp.localAnchor1.y - lc->y;
                double tX = R->col1.x * r1X + R->col2.x * r1Y;
                r1Y = R->col1.y * r1X + R->col2.y * r1Y;
                r1X = tX;
                R = &b2.xf.R;
                lc = &b2.sweep.localCenter;
                double r2X = ccp.localAnchor2.x - lc->x;
                double r2Y = ccp.localAnchor2.y - lc->y;
                tX = R->col1.x * r2X + R->col2.x * r2Y;
                r2Y = R->col1.y * r2X + R->col2.y * r2Y;
                r2X = tX;
                double p1X = b1_c.x + r1X;
                double p1Y = b1_c.y + r1Y;
                double p2X = b2_c.x + r2X;
                double p2Y = b2_c.y + r2Y;
                double dpX = p2X - p1X;
                double dpY = p2Y - p1Y;
                double separation = dpX * normalX + dpY * normalY + ccp.separation;
                minSeparation = b2Min(minSeparation, separation);
                double C = baumgarte *
                           b2Clamp(separation + settings::linearSlop, -settings::maxLinearCorrection, 0);
                double dImpulse = -ccp.equalizedMass * C;
                double impulse0 = ccp.positionImpulse;
                ccp.positionImpulse = b2Max(impulse0 + dImpulse, 0);
                dImpulse = ccp.positionImpulse - impulse0;
                double impulseX = dImpulse * normalX;
                double impulseY = dImpulse * normalY;
                b1_c.x -= invMass1 * impulseX;
                b1_c.y -= invMass1 * impulseY;
                b1_a -= invI1 * (r1X * impulseY - r1Y * impulseX);
                b1.sweep.a = b1_a;
                w.SynchronizeTransform(b1i);
                b2_c.x += invMass2 * impulseX;
                b2_c.y += invMass2 * impulseY;
                b2_a += invI2 * (r2X * impulseY - r2Y * impulseX);
                b2.sweep.a = b2_a;
                w.SynchronizeTransform(b2i);
            }
        }
        return minSeparation >= -1.5 * settings::linearSlop;
    }
};

// thread-local scratch (never part of a snapshot)
struct Scratch {
    std::vector<int32_t> stack, islandBodies, islandContacts, islandJoints;
    std::vector<ContactConstraint> constraints;
};
thread_local Scratch g_scratch;

}  // namespace

// ============================================================ island

namespace {
struct Island {
    World& w;
    int32_t* bodies;
    int32_t* contacts;
    int32_t* joints;
    int32_t bodyCount = 0, contactCount = 0, jointCount = 0;
    int32_t bodyCapacity, contactCapacity, jointCapacity;
    int32_t positionIterationCount = 0;

    Island(World& world, int32_t bodyCap, int32_t contactCap, int32_t jointCap)
        : w(world), bodyCapacity(bodyCap), contactCapacity(contactCap), jointCapacity(jointCap) {
        Scratch& S = g_scratch;
        if ((int32_t)S.islandBodies.size() < bodyCap) S.islandBodies.resize((size_t)bodyCap);
        if ((int32_t)S.islandContacts.size() < contactCap) S.islandContacts.resize((size_t)contactCap);
        if ((int32_t)S.islandJoints.size() < jointCap + 1) S.islandJoints.resize((size_t)jointCap + 1);
        bodies = S.islandBodies.data();
        contacts = S.islandContacts.data();
        joints = S.islandJoints.data();
    }
    void Clear() { bodyCount = contactCount = jointCount = 0; }
    void AddBody(int32_t b) {
        if (bodyCount >= bodyCapacity) fatal("island body overflow");
        bodies[bodyCount++] = b;
    }
    void AddContact(int32_t c) {
        if (contactCount >= contactCapacity) fatal("island contact overflow");
        contacts[contactCount++] = c;
    }
    void AddJoint(int32_t j) {
        if (jointCount >= jointCapacity) fatal("island joint overflow");
        joints[jointCount++] = j;
    }

    void Solve(const TimeStep& step, const Vec2& gravity, bool correctPositions, bool allowSleep) {
        for (int32_t i = 0; i < bodyCount; ++i) {
            Body& b = w.bodies[bodies[i]];
            if (b.IsStatic()) continue;
            b.linearVelocity.x += step.dt * (gravity.x + b.invMass * b.force.x);
            b.linearVelocity.y += step.dt * (gravity.y + b.invMass * b.force.y);
            b.angularVelocity += step.dt * b.invI * b.torque;
            b.force.SetZero();
            b.torque = 0;
            b.linearVelocity.Multiply(b2Clamp(1 - step.dt * b.linearDamping, 0, 1));
            b.angularVelocity *= b2Clamp(1 - step.dt * b.angularDamping, 0, 1);
            if (b.linearVelocity.LengthSquared() > settings::maxLinearVelocitySquared) {
                b.linearVelocity.Normalize();
                b.linearVelocity.x *= settings::maxLinearVelocity;
                b.linearVelocity.y *= settings::maxLinearVelocity;
            }
            if (b.angularVelocity * b.angularVelocity > settings::maxAngularVelocitySquared) {
                if (b.angularVelocity < 0)
                    b.angularVelocity = -settings::maxAngularVelocity;
                else
                    b.angularVelocity = settings::maxAngularVelocity;
            }
        }
        ContactSolver contactSolver(w, step, contacts, contactCount, g_scratch.constraints);
        contactSolver.InitVelocityConstraints(step);
        if (jointCount > 0) fatal("joint solving not implemented yet");
        for (int32_t i = 0; i < step.maxIterations; ++i) {
            contactSolver.SolveVelocityConstraints();
            // joints SolveVelocityConstraints (next milestone)
        }
        contactSolver.FinalizeVelocityConstraints();
        for (int32_t i = 0; i < bodyCount; ++i) {
            int32_t bi = bodies[i];
            Body& b = w.bodies[bi];
            if (b.IsStatic()) continue;
            b.sweep.c0.SetV(b.sweep.c);
            b.sweep.a0 = b.sweep.a;
            b.sweep.c.x += step.dt * b.linearVelocity.x;
            b.sweep.c.y += step.dt * b.linearVelocity.y;
            b.sweep.a += step.dt * b.angularVelocity;
            w.SynchronizeTransform(bi);
        }
        if (correctPositions) {
            // joints InitPositionConstraints (next milestone)
            for (positionIterationCount = 0; positionIterationCount < step.maxIterations; ++positionIterationCount) {
                bool contactsOkay = contactSolver.SolvePositionConstraints(settings::contactBaumgarte);
                bool jointsOkay = true;
                if (contactsOkay && jointsOkay) break;
            }
        }
        // Report(): listener.Result is sound-only in this game
        if (allowSleep) {
            double minSleepTime = NUM_MAX_VALUE;
            const double linTolSqr = settings::linearSleepTolerance * settings::linearSleepTolerance;
            const double angTolSqr = settings::angularSleepTolerance * settings::angularSleepTolerance;
            for (int32_t i = 0; i < bodyCount; ++i) {
                Body& b = w.bodies[bodies[i]];
                if (b.invMass == 0) continue;
                if ((b.flags & BF_ALLOWSLEEP) == 0) {
                    b.sleepTime = 0;
                    minSleepTime = 0;
                }
                if ((b.flags & BF_ALLOWSLEEP) == 0 || b.angularVelocity * b.angularVelocity > angTolSqr ||
                    b2Dot(b.linearVelocity, b.linearVelocity) > linTolSqr) {
                    b.sleepTime = 0;
                    minSleepTime = 0;
                } else {
                    b.sleepTime += step.dt;
                    minSleepTime = b2Min(minSleepTime, b.sleepTime);
                }
            }
            if (minSleepTime >= settings::timeToSleep) {
                for (int32_t i = 0; i < bodyCount; ++i) {
                    Body& b = w.bodies[bodies[i]];
                    b.flags |= BF_SLEEP;
                    b.linearVelocity.SetZero();
                    b.angularVelocity = 0;
                }
            }
        }
    }

    void SolveTOI(const TimeStep& subStep) {
        ContactSolver contactSolver(w, subStep, contacts, contactCount, g_scratch.constraints);
        for (int32_t i = 0; i < subStep.maxIterations; ++i) contactSolver.SolveVelocityConstraints();
        for (int32_t i = 0; i < bodyCount; ++i) {
            int32_t bi = bodies[i];
            Body& b = w.bodies[bi];
            if (b.IsStatic()) continue;
            b.sweep.c0.SetV(b.sweep.c);
            b.sweep.a0 = b.sweep.a;
            b.sweep.c.x += subStep.dt * b.linearVelocity.x;
            b.sweep.c.y += subStep.dt * b.linearVelocity.y;
            b.sweep.a += subStep.dt * b.angularVelocity;
            w.SynchronizeTransform(bi);
        }
        const double k_toiBaumgarte = 0.75;
        for (int32_t i = 0; i < subStep.maxIterations; ++i) {
            bool contactsOkay = contactSolver.SolvePositionConstraints(k_toiBaumgarte);
            if (contactsOkay) break;
        }
        // Report(): sound-only
    }
};
}  // namespace

// ============================================================ world step

void World::Solve(const TimeStep& step) {
    positionIterationCount = 0;
    Island island(*this, bodyCount, contactCount, jointCount);
    for (int32_t b = bodyList; b != -1; b = bodies[b].next) bodies[b].flags &= ~BF_ISLAND;
    for (int32_t c = contactList; c != -1; c = contacts[c].next) contacts[c].flags &= ~CF_ISLAND;
    for (int32_t j = jointList; j != -1; j = joints[j].next) joints[j].islandFlag = false;

    std::vector<int32_t>& stack = g_scratch.stack;
    if ((int32_t)stack.size() < CAP_BODIES) stack.resize(CAP_BODIES);
    for (int32_t seed = bodyList; seed != -1; seed = bodies[seed].next) {
        if (bodies[seed].flags & (BF_ISLAND | BF_SLEEP | BF_FROZEN)) continue;
        if (bodies[seed].IsStatic()) continue;
        island.Clear();
        int32_t stackCount = 0;
        stack[(size_t)stackCount++] = seed;
        bodies[seed].flags |= BF_ISLAND;
        while (stackCount > 0) {
            int32_t b = stack[(size_t)--stackCount];
            island.AddBody(b);
            bodies[b].flags &= ~BF_SLEEP;
            if (bodies[b].IsStatic()) continue;
            for (int32_t ce = bodies[b].contactList; ce != -1; ce = CEdge(*this, ce).next) {
                Contact& contact = contacts[ce >> 1];
                if (contact.flags & (CF_ISLAND | CF_NONSOLID)) continue;
                if (contact.manifoldCount == 0) continue;
                island.AddContact(ce >> 1);
                contact.flags |= CF_ISLAND;
                int32_t other = CEdge(*this, ce).other;
                if (bodies[other].flags & BF_ISLAND) continue;
                if (stackCount >= CAP_BODIES) fatal("island stack overflow");
                stack[(size_t)stackCount++] = other;
                bodies[other].flags |= BF_ISLAND;
            }
            for (int32_t je = bodies[b].jointList; je != -1; je = JEdge(*this, je).next) {
                Joint& joint = joints[je >> 1];
                if (joint.islandFlag == true) continue;
                island.AddJoint(je >> 1);
                joint.islandFlag = true;
                int32_t other = JEdge(*this, je).other;
                if (bodies[other].flags & BF_ISLAND) continue;
                if (stackCount >= CAP_BODIES) fatal("island stack overflow");
                stack[(size_t)stackCount++] = other;
                bodies[other].flags |= BF_ISLAND;
            }
        }
        island.Solve(step, gravity, positionCorrection, allowSleep);
        if (island.positionIterationCount > positionIterationCount)
            positionIterationCount = island.positionIterationCount;
        for (int32_t i = 0; i < island.bodyCount; ++i) {
            Body& b = bodies[island.bodies[i]];
            if (b.IsStatic()) b.flags &= ~BF_ISLAND;
        }
    }
    for (int32_t b = bodyList; b != -1; b = bodies[b].next) {
        if (bodies[b].flags & (BF_SLEEP | BF_FROZEN)) continue;
        if (bodies[b].IsStatic()) continue;
        SynchronizeShapes(b);  // boundary listener is null in this game
    }
    BP_Commit();
}

void World::SolveTOI(const TimeStep& step) {
    Island island(*this, bodyCount, settings::maxTOIContactsPerIsland, 0);
    std::vector<int32_t>& queue = g_scratch.stack;
    if ((int32_t)queue.size() < CAP_BODIES) queue.resize(CAP_BODIES);
    for (int32_t b = bodyList; b != -1; b = bodies[b].next) {
        bodies[b].flags &= ~BF_ISLAND;
        bodies[b].sweep.t0 = 0;
    }
    for (int32_t c = contactList; c != -1; c = contacts[c].next) contacts[c].flags &= ~(CF_TOI | CF_ISLAND);

    for (;;) {
        int32_t minContact = -1;
        double minTOI = 1;
        for (int32_t c = contactList; c != -1; c = contacts[c].next) {
            Contact& ct = contacts[c];
            if (ct.flags & (CF_SLOW | CF_NONSOLID)) continue;
            double toi = 1;
            if (ct.flags & CF_TOI) {
                toi = ct.toi;
            } else {
                const Shape& s1 = shapes[ct.shape1];
                const Shape& s2 = shapes[ct.shape2];
                int32_t b1i = s1.body, b2i = s2.body;
                Body& b1 = bodies[b1i];
                Body& b2 = bodies[b2i];
                if ((b1.IsStatic() || b1.IsSleeping()) && (b2.IsStatic() || b2.IsSleeping())) continue;
                double t0 = b1.sweep.t0;
                if (b1.sweep.t0 < b2.sweep.t0) {
                    t0 = b2.sweep.t0;
                    b1.sweep.Advance(t0);
                } else if (b2.sweep.t0 < b1.sweep.t0) {
                    t0 = b1.sweep.t0;
                    b2.sweep.Advance(t0);
                }
                toi = TimeOfImpact((*geoms)[s1.geom], s1, b1.sweep, (*geoms)[s2.geom], s2, b2.sweep);
                if (toi > 0 && toi < 1) {
                    toi = (1 - toi) * t0 + toi;
                    if (toi > 1) toi = 1;
                }
                ct.toi = toi;
                ct.flags |= CF_TOI;
            }
            if (NUM_MIN_VALUE < toi && toi < minTOI) {
                minContact = c;
                minTOI = toi;
            }
        }
        if (minContact == -1 || 1 - 100 * NUM_MIN_VALUE < minTOI) break;

        int32_t b1i = shapes[contacts[minContact].shape1].body;
        int32_t b2i = shapes[contacts[minContact].shape2].body;
        Advance(b1i, minTOI);
        Advance(b2i, minTOI);
        ContactUpdate(minContact);
        contacts[minContact].flags &= ~CF_TOI;
        if (contacts[minContact].manifoldCount == 0) continue;

        int32_t seed = b1i;
        if (bodies[seed].IsStatic()) seed = b2i;
        island.Clear();
        int32_t queueSize = 0;
        queue[(size_t)queueSize++] = seed;
        bodies[seed].flags |= BF_ISLAND;
        while (queueSize > 0) {
            int32_t b = queue[(size_t)--queueSize];
            island.AddBody(b);
            bodies[b].flags &= ~BF_SLEEP;
            if (bodies[b].IsStatic()) continue;
            for (int32_t ce = bodies[b].contactList; ce != -1; ce = CEdge(*this, ce).next) {
                if (island.contactCount == island.contactCapacity) continue;
                Contact& contact = contacts[ce >> 1];
                if (contact.flags & (CF_ISLAND | CF_SLOW | CF_NONSOLID)) continue;
                if (contact.manifoldCount == 0) continue;
                island.AddContact(ce >> 1);
                contact.flags |= CF_ISLAND;
                int32_t other = CEdge(*this, ce).other;
                if (bodies[other].flags & BF_ISLAND) continue;
                if (bodies[other].IsStatic() == false) {
                    Advance(other, minTOI);
                    bodies[other].WakeUp();
                }
                if (queueSize >= CAP_BODIES) fatal("TOI queue overflow");
                queue[(size_t)queueSize++] = other;
                bodies[other].flags |= BF_ISLAND;
            }
        }
        TimeStep subStep;
        subStep.dt = (1 - minTOI) * step.dt;
        subStep.inv_dt = 1 / subStep.dt;
        subStep.maxIterations = step.maxIterations;
        island.SolveTOI(subStep);
        for (int32_t i = 0; i < island.bodyCount; ++i) {
            int32_t bi = island.bodies[i];
            Body& b = bodies[bi];
            b.flags &= ~BF_ISLAND;
            if (b.flags & (BF_SLEEP | BF_FROZEN)) continue;
            if (b.IsStatic()) continue;
            SynchronizeShapes(bi);
            for (int32_t ce = bodies[bi].contactList; ce != -1; ce = CEdge(*this, ce).next)
                contacts[ce >> 1].flags &= ~CF_TOI;
        }
        for (int32_t i = 0; i < island.contactCount; ++i)
            contacts[island.contacts[i]].flags &= ~(CF_TOI | CF_ISLAND);
        BP_Commit();
    }
}

void World::Step(double dt, int32_t iterations) {
    lock = true;
    TimeStep step;
    step.dt = dt;
    step.maxIterations = iterations;
    step.inv_dt = dt > 0 ? 1 / dt : 0;
    step.dtRatio = inv_dt0 * dt;
    step.positionCorrection = positionCorrection;
    step.warmStarting = warmStarting;
    CollideAll();
    if (step.dt > 0) Solve(step);
    if (continuousPhysics && step.dt > 0) SolveTOI(step);
    // DrawDebugData: rendering only
    inv_dt0 = step.inv_dt;
    lock = false;
}

}  // namespace rb
