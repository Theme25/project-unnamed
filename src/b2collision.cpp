// b2collision.cpp - shape geometry, narrow phase, GJK distance and TOI.
// Transliterated from Box2D/Collision/*.as of the Red Ball 1 SWF.
#include "b2world.h"
#include <cstdio>
#include <cstdlib>

namespace rb {

double (*g_sinHook)(double) = nullptr;
double (*g_cosHook)(double) = nullptr;

void fatal(const char* msg) {
    std::fprintf(stderr, "rbsim fatal: %s\n", msg);
    std::abort();
}

// ======================================================================
// Geometry construction (b2CircleShape / b2PolygonShape constructors)
// ======================================================================

static Vec2 ComputeCentroid(const Vec2* vs, int count) {
    Vec2 c;
    double area = 0;
    double p1X = 0, p1Y = 0;
    double inv3 = 1.0 / 3.0;
    for (int i = 0; i < count; ++i) {
        const Vec2& p2 = vs[i];
        const Vec2& p3 = i + 1 < count ? vs[i + 1] : vs[0];
        double e1X = p2.x - p1X;
        double e1Y = p2.y - p1Y;
        double e2X = p3.x - p1X;
        double e2Y = p3.y - p1Y;
        double D = e1X * e2Y - e1Y * e2X;
        double triangleArea = 0.5 * D;
        area += triangleArea;
        c.x += triangleArea * inv3 * (p1X + p2.x + p3.x);
        c.y += triangleArea * inv3 * (p1Y + p2.y + p3.y);
    }
    c.x *= 1 / area;
    c.y *= 1 / area;
    return c;
}

static void ComputeOBB(OBB& obb, const Vec2* vs, int count) {
    Vec2 p[MAX_POLY_VERTS + 1];
    for (int i = 0; i < count; ++i) p[i] = vs[i];
    p[count] = p[0];
    double minArea = NUM_MAX_VALUE;
    // initial obb.R from `new b2Mat22()` == Set(0) -> identity
    obb.R.Set(0);
    for (int i = 1; i <= count; ++i) {
        const Vec2 root = p[i - 1];
        double uxX = p[i].x - root.x;
        double uxY = p[i].y - root.y;
        double length = as3_sqrt(uxX * uxX + uxY * uxY);
        uxX /= length;
        uxY /= length;
        double uyX = -uxY;
        double uyY = uxX;
        double lowerX = NUM_MAX_VALUE, lowerY = NUM_MAX_VALUE;
        double upperX = -NUM_MAX_VALUE, upperY = -NUM_MAX_VALUE;
        for (int j = 0; j < count; ++j) {
            double dX = p[j].x - root.x;
            double dY = p[j].y - root.y;
            double rX = uxX * dX + uxY * dY;
            double rY = uyX * dX + uyY * dY;
            if (rX < lowerX) lowerX = rX;
            if (rY < lowerY) lowerY = rY;
            if (rX > upperX) upperX = rX;
            if (rY > upperY) upperY = rY;
        }
        double area = (upperX - lowerX) * (upperY - lowerY);
        if (area < 0.95 * minArea) {
            minArea = area;
            obb.R.col1.x = uxX;
            obb.R.col1.y = uxY;
            obb.R.col2.x = uyX;
            obb.R.col2.y = uyY;
            double centerX = 0.5 * (lowerX + upperX);
            double centerY = 0.5 * (lowerY + upperY);
            const Mat22& R = obb.R;
            obb.center.x = root.x + (R.col1.x * centerX + R.col2.x * centerY);
            obb.center.y = root.y + (R.col1.y * centerX + R.col2.y * centerY);
            obb.extents.x = 0.5 * (upperX - lowerX);
            obb.extents.y = 0.5 * (upperY - lowerY);
        }
    }
}

int32_t GeomTable::Add(const ShapeDef& def) {
    Geom g;
    g.type = def.type;
    if (def.type == e_circleShape) {
        g.localPosition = def.localPosition;
        g.radius = def.radius;
    } else if (def.type == e_polygonShape) {
        int n = def.vertexCount;
        if (n < 3 || n > MAX_POLY_VERTS) fatal("bad polygon vertex count");
        g.vertexCount = n;
        for (int i = 0; i < n; ++i) g.vertices[i] = def.vertices[i];
        for (int i = 0; i < n; ++i) {
            int i1 = i;
            int i2 = i + 1 < n ? i + 1 : 0;
            double edgeX = g.vertices[i2].x - g.vertices[i1].x;
            double edgeY = g.vertices[i2].y - g.vertices[i1].y;
            double len = as3_sqrt(edgeX * edgeX + edgeY * edgeY);
            g.normals[i] = Vec2(edgeY / len, -edgeX / len);
        }
        g.centroid = ComputeCentroid(def.vertices, def.vertexCount);
        ComputeOBB(g.obb, g.vertices, n);
        for (int i = 0; i < n; ++i) {
            int i1 = i - 1 >= 0 ? i - 1 : n - 1;
            int i2 = i;
            double n1X = g.normals[i1].x;
            double n1Y = g.normals[i1].y;
            double n2X = g.normals[i2].x;
            double n2Y = g.normals[i2].y;
            double vX = g.vertices[i].x - g.centroid.x;
            double vY = g.vertices[i].y - g.centroid.y;
            double dX = n1X * vX + n1Y * vY - settings::toiSlop;
            double dY = n2X * vX + n2Y * vY - settings::toiSlop;
            double det = 1 / (n1X * n2Y - n1Y * n2X);
            g.coreVertices[i] = Vec2(det * (n2Y * dX - n1Y * dY) + g.centroid.x,
                                     det * (n1X * dY - n2X * dX) + g.centroid.y);
        }
    } else {
        fatal("unknown shape type");
    }
    geoms.push_back(g);
    return (int32_t)geoms.size() - 1;
}

// ======================================================================
// Shape queries
// ======================================================================

void ComputeAABB(const Geom& g, AABB& aabb, const XForm& xf) {
    if (g.type == e_circleShape) {
        const Mat22& R = xf.R;
        const Vec2& lp = g.localPosition;
        double pX = xf.position.x + (R.col1.x * lp.x + R.col2.x * lp.y);
        double pY = xf.position.y + (R.col1.y * lp.x + R.col2.y * lp.y);
        aabb.lowerBound.Set(pX - g.radius, pY - g.radius);
        aabb.upperBound.Set(pX + g.radius, pY + g.radius);
    } else {
        Mat22 m;
        const Mat22& R = xf.R;
        const Vec2* col = &g.obb.R.col1;
        m.col1.x = R.col1.x * col->x + R.col2.x * col->y;
        m.col1.y = R.col1.y * col->x + R.col2.y * col->y;
        col = &g.obb.R.col2;
        m.col2.x = R.col1.x * col->x + R.col2.x * col->y;
        m.col2.y = R.col1.y * col->x + R.col2.y * col->y;
        m.Abs();
        const Vec2& ext = g.obb.extents;
        double hX = m.col1.x * ext.x + m.col2.x * ext.y;
        double hY = m.col1.y * ext.x + m.col2.y * ext.y;
        const Vec2& ctr = g.obb.center;
        double pX = xf.position.x + (R.col1.x * ctr.x + R.col2.x * ctr.y);
        double pY = xf.position.y + (R.col1.y * ctr.x + R.col2.y * ctr.y);
        aabb.lowerBound.Set(pX - hX, pY - hY);
        aabb.upperBound.Set(pX + hX, pY + hY);
    }
}

void ComputeSweptAABB(const Geom& g, AABB& aabb, const XForm& xf1, const XForm& xf2) {
    if (g.type == e_circleShape) {
        const Vec2& lp = g.localPosition;
        const Mat22* R = &xf1.R;
        double p1X = xf1.position.x + (R->col1.x * lp.x + R->col2.x * lp.y);
        double p1Y = xf1.position.y + (R->col1.y * lp.x + R->col2.y * lp.y);
        R = &xf2.R;
        double p2X = xf2.position.x + (R->col1.x * lp.x + R->col2.x * lp.y);
        double p2Y = xf2.position.y + (R->col1.y * lp.x + R->col2.y * lp.y);
        aabb.lowerBound.Set((p1X < p2X ? p1X : p2X) - g.radius, (p1Y < p2Y ? p1Y : p2Y) - g.radius);
        aabb.upperBound.Set((p1X > p2X ? p1X : p2X) + g.radius, (p1Y > p2Y ? p1Y : p2Y) + g.radius);
    } else {
        AABB a1, a2;
        ComputeAABB(g, a1, xf1);
        ComputeAABB(g, a2, xf2);
        aabb.lowerBound.Set(a1.lowerBound.x < a2.lowerBound.x ? a1.lowerBound.x : a2.lowerBound.x,
                            a1.lowerBound.y < a2.lowerBound.y ? a1.lowerBound.y : a2.lowerBound.y);
        aabb.upperBound.Set(a1.upperBound.x > a2.upperBound.x ? a1.upperBound.x : a2.upperBound.x,
                            a1.upperBound.y > a2.upperBound.y ? a1.upperBound.y : a2.upperBound.y);
    }
}

void ComputeMass(const Geom& g, double density, MassData& md) {
    if (g.type == e_circleShape) {
        md.mass = density * settings::pi * g.radius * g.radius;
        md.center = g.localPosition;
        md.I = md.mass * (0.5 * g.radius * g.radius +
                          (g.localPosition.x * g.localPosition.x + g.localPosition.y * g.localPosition.y));
        return;
    }
    double centerX = 0, centerY = 0, area = 0, I = 0;
    double p1X = 0, p1Y = 0;
    double k_inv3 = 1.0 / 3.0;
    int n = g.vertexCount;
    for (int i = 0; i < n; ++i) {
        const Vec2& p2 = g.vertices[i];
        const Vec2& p3 = i + 1 < n ? g.vertices[i + 1] : g.vertices[0];
        double e1X = p2.x - p1X;
        double e1Y = p2.y - p1Y;
        double e2X = p3.x - p1X;
        double e2Y = p3.y - p1Y;
        double D = e1X * e2Y - e1Y * e2X;
        double triangleArea = 0.5 * D;
        area += triangleArea;
        centerX += triangleArea * k_inv3 * (p1X + p2.x + p3.x);
        centerY += triangleArea * k_inv3 * (p1Y + p2.y + p3.y);
        double px = p1X, py = p1Y;
        double ex1 = e1X, ey1 = e1Y;
        double ex2 = e2X, ey2 = e2Y;
        double intx2 = k_inv3 * (0.25 * (ex1 * ex1 + ex2 * ex1 + ex2 * ex2) + (px * ex1 + px * ex2)) + 0.5 * px * px;
        double inty2 = k_inv3 * (0.25 * (ey1 * ey1 + ey2 * ey1 + ey2 * ey2) + (py * ey1 + py * ey2)) + 0.5 * py * py;
        I += D * (intx2 + inty2);
    }
    md.mass = density * area;
    centerX *= 1 / area;
    centerY *= 1 / area;
    md.center.Set(centerX, centerY);
    md.I = density * I;
}

double UpdateSweepRadius(const Geom& g, const Vec2& center) {
    if (g.type == e_circleShape) {
        double dX = g.localPosition.x - center.x;
        double dY = g.localPosition.y - center.y;
        dX = as3_sqrt(dX * dX + dY * dY);
        return dX + g.radius - settings::toiSlop;
    }
    double r = 0;
    for (int i = 0; i < g.vertexCount; ++i) {
        const Vec2& v = g.coreVertices[i];
        double dX = v.x - center.x;
        double dY = v.y - center.y;
        dX = as3_sqrt(dX * dX + dY * dY);
        if (dX > r) r = dX;
    }
    return r;
}

static bool TestPointGeom(const Geom& g, const XForm& xf, const Vec2& p) {
    if (g.type == e_circleShape) {
        const Mat22& R = xf.R;
        const Vec2& lp = g.localPosition;
        double dX = xf.position.x + (R.col1.x * lp.x + R.col2.x * lp.y);
        double dY = xf.position.y + (R.col1.y * lp.x + R.col2.y * lp.y);
        dX = p.x - dX;
        dY = p.y - dY;
        return dX * dX + dY * dY <= g.radius * g.radius;
    }
    const Mat22& R = xf.R;
    double tX = p.x - xf.position.x;
    double tY = p.y - xf.position.y;
    double pLocalX = tX * R.col1.x + tY * R.col1.y;
    double pLocalY = tX * R.col2.x + tY * R.col2.y;
    for (int i = 0; i < g.vertexCount; ++i) {
        tX = pLocalX - g.vertices[i].x;
        tY = pLocalY - g.vertices[i].y;
        double dot = g.normals[i].x * tX + g.normals[i].y * tY;
        if (dot > 0) return false;
    }
    return true;
}

bool World::ShapeTestPoint(int32_t s, const XForm& xf, const Vec2& p) const {
    return TestPointGeom((*geoms)[shapes[s].geom], xf, p);
}

// ======================================================================
// Narrow phase
// ======================================================================

void CollideCircles(Manifold& m, const Geom& c1, const XForm& xf1, const Geom& c2, const XForm& xf2) {
    m.pointCount = 0;
    const Mat22* R = &xf1.R;
    const Vec2* lp = &c1.localPosition;
    double p1X = xf1.position.x + (R->col1.x * lp->x + R->col2.x * lp->y);
    double p1Y = xf1.position.y + (R->col1.y * lp->x + R->col2.y * lp->y);
    R = &xf2.R;
    lp = &c2.localPosition;
    double p2X = xf2.position.x + (R->col1.x * lp->x + R->col2.x * lp->y);
    double p2Y = xf2.position.y + (R->col1.y * lp->x + R->col2.y * lp->y);
    double dX = p2X - p1X;
    double dY = p2Y - p1Y;
    double distSqr = dX * dX + dY * dY;
    double r1 = c1.radius;
    double r2 = c2.radius;
    double radiusSum = r1 + r2;
    if (distSqr > radiusSum * radiusSum) return;
    double separation;
    if (distSqr < NUM_MIN_VALUE) {
        separation = -radiusSum;
        m.normal.Set(0, 1);
    } else {
        double dist = as3_sqrt(distSqr);
        separation = dist - radiusSum;
        double a = 1 / dist;
        m.normal.x = a * dX;
        m.normal.y = a * dY;
    }
    m.pointCount = 1;
    ManifoldPoint& mp = m.points[0];
    mp.key = 0;
    mp.separation = separation;
    p1X += r1 * m.normal.x;
    p1Y += r1 * m.normal.y;
    p2X -= r2 * m.normal.x;
    p2Y -= r2 * m.normal.y;
    double pX = 0.5 * (p1X + p2X);
    double pY = 0.5 * (p1Y + p2Y);
    double tX = pX - xf1.position.x;
    double tY = pY - xf1.position.y;
    mp.localPoint1.x = tX * xf1.R.col1.x + tY * xf1.R.col1.y;
    mp.localPoint1.y = tX * xf1.R.col2.x + tY * xf1.R.col2.y;
    tX = pX - xf2.position.x;
    tY = pY - xf2.position.y;
    mp.localPoint2.x = tX * xf2.R.col1.x + tY * xf2.R.col1.y;
    mp.localPoint2.y = tX * xf2.R.col2.x + tY * xf2.R.col2.y;
}

void CollidePolygonAndCircle(Manifold& m, const Geom& poly, const XForm& xf1, const Geom& circle,
                             const XForm& xf2) {
    m.pointCount = 0;
    const Mat22* R = &xf2.R;
    const Vec2* t = &circle.localPosition;
    double cX = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double cY = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    double dX = cX - xf1.position.x;
    double dY = cY - xf1.position.y;
    R = &xf1.R;
    double cLocalX = dX * R->col1.x + dY * R->col1.y;
    double cLocalY = dX * R->col2.x + dY * R->col2.y;
    int normalIndex = 0;
    double separation = -NUM_MAX_VALUE;
    double radius = circle.radius;
    int vertexCount = poly.vertexCount;
    const Vec2* vertices = poly.vertices;
    const Vec2* normals = poly.normals;
    for (int i = 0; i < vertexCount; ++i) {
        t = &vertices[i];
        dX = cLocalX - t->x;
        dY = cLocalY - t->y;
        t = &normals[i];
        double s = t->x * dX + t->y * dY;
        if (s > radius) return;
        if (s > separation) {
            separation = s;
            normalIndex = i;
        }
    }
    ManifoldPoint& tPoint = m.points[0];
    if (separation < NUM_MIN_VALUE) {
        m.pointCount = 1;
        t = &normals[normalIndex];
        R = &xf1.R;
        m.normal.x = R->col1.x * t->x + R->col2.x * t->y;
        m.normal.y = R->col1.y * t->x + R->col2.y * t->y;
        id_setIncidentEdge(tPoint.key, (uint32_t)normalIndex);
        id_setIncidentVertex(tPoint.key, 255);  // b2_nullFeature
        double positionX = cX - radius * m.normal.x;
        double positionY = cY - radius * m.normal.y;
        dX = positionX - xf1.position.x;
        dY = positionY - xf1.position.y;
        R = &xf1.R;
        tPoint.localPoint1.x = dX * R->col1.x + dY * R->col1.y;
        tPoint.localPoint1.y = dX * R->col2.x + dY * R->col2.y;
        dX = positionX - xf2.position.x;
        dY = positionY - xf2.position.y;
        R = &xf2.R;
        tPoint.localPoint2.x = dX * R->col1.x + dY * R->col1.y;
        tPoint.localPoint2.y = dX * R->col2.x + dY * R->col2.y;
        tPoint.separation = separation - radius;
        return;
    }
    int vertIndex1 = normalIndex;
    int vertIndex2 = vertIndex1 + 1 < vertexCount ? vertIndex1 + 1 : 0;
    const Vec2& v1 = vertices[vertIndex1];
    const Vec2& v2 = vertices[vertIndex2];
    double eX = v2.x - v1.x;
    double eY = v2.y - v1.y;
    double length = as3_sqrt(eX * eX + eY * eY);
    eX /= length;
    eY /= length;
    dX = cLocalX - v1.x;
    dY = cLocalY - v1.y;
    double u = dX * eX + dY * eY;
    double pX, pY;
    if (u <= 0) {
        pX = v1.x;
        pY = v1.y;
        id_setIncidentEdge(tPoint.key, 255);
        id_setIncidentVertex(tPoint.key, (uint32_t)vertIndex1);
    } else if (u >= length) {
        pX = v2.x;
        pY = v2.y;
        id_setIncidentEdge(tPoint.key, 255);
        id_setIncidentVertex(tPoint.key, (uint32_t)vertIndex2);
    } else {
        pX = eX * u + v1.x;
        pY = eY * u + v1.y;
        id_setIncidentEdge(tPoint.key, (uint32_t)normalIndex);
        id_setIncidentVertex(tPoint.key, 255);
    }
    dX = cLocalX - pX;
    dY = cLocalY - pY;
    double dist = as3_sqrt(dX * dX + dY * dY);
    dX /= dist;
    dY /= dist;
    if (dist > radius) return;
    m.pointCount = 1;
    R = &xf1.R;
    m.normal.x = R->col1.x * dX + R->col2.x * dY;
    m.normal.y = R->col1.y * dX + R->col2.y * dY;
    double positionX = cX - radius * m.normal.x;
    double positionY = cY - radius * m.normal.y;
    dX = positionX - xf1.position.x;
    dY = positionY - xf1.position.y;
    R = &xf1.R;
    tPoint.localPoint1.x = dX * R->col1.x + dY * R->col1.y;
    tPoint.localPoint1.y = dX * R->col2.x + dY * R->col2.y;
    dX = positionX - xf2.position.x;
    dY = positionY - xf2.position.y;
    R = &xf2.R;
    tPoint.localPoint2.x = dX * R->col1.x + dY * R->col1.y;
    tPoint.localPoint2.y = dX * R->col2.x + dY * R->col2.y;
    tPoint.separation = dist - radius;
}

struct ClipVertex {
    Vec2 v;
    uint32_t key = 0;
};

static double EdgeSeparation(const Geom& poly1, const XForm& xf1, int edge1, const Geom& poly2,
                             const XForm& xf2) {
    int count2 = poly2.vertexCount;
    const Vec2* vertices2 = poly2.vertices;
    const Mat22* R = &xf1.R;
    const Vec2* t = &poly1.normals[edge1];
    double normal1WorldX = R->col1.x * t->x + R->col2.x * t->y;
    double normal1WorldY = R->col1.y * t->x + R->col2.y * t->y;
    R = &xf2.R;
    double normal1X = R->col1.x * normal1WorldX + R->col1.y * normal1WorldY;
    double normal1Y = R->col2.x * normal1WorldX + R->col2.y * normal1WorldY;
    int index = 0;
    double minDot = NUM_MAX_VALUE;
    for (int i = 0; i < count2; ++i) {
        t = &vertices2[i];
        double dot = t->x * normal1X + t->y * normal1Y;
        if (dot < minDot) {
            minDot = dot;
            index = i;
        }
    }
    t = &poly1.vertices[edge1];
    R = &xf1.R;
    double v1X = xf1.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double v1Y = xf1.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    t = &vertices2[index];
    R = &xf2.R;
    double v2X = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double v2Y = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    v2X -= v1X;
    v2Y -= v1Y;
    return v2X * normal1WorldX + v2Y * normal1WorldY;
}

static void FindIncidentEdge(ClipVertex c[2], const Geom& poly1, const XForm& xf1, int edge1,
                             const Geom& poly2, const XForm& xf2) {
    int count2 = poly2.vertexCount;
    const Vec2* vertices2 = poly2.vertices;
    const Vec2* normals2 = poly2.normals;
    const Mat22* R = &xf1.R;
    const Vec2* t = &poly1.normals[edge1];
    double normal1X = R->col1.x * t->x + R->col2.x * t->y;
    double normal1Y = R->col1.y * t->x + R->col2.y * t->y;
    R = &xf2.R;
    double tX = R->col1.x * normal1X + R->col1.y * normal1Y;
    normal1Y = R->col2.x * normal1X + R->col2.y * normal1Y;
    normal1X = tX;
    int index = 0;
    double minDot = NUM_MAX_VALUE;
    for (int i = 0; i < count2; ++i) {
        t = &normals2[i];
        double dot = normal1X * t->x + normal1Y * t->y;
        if (dot < minDot) {
            minDot = dot;
            index = i;
        }
    }
    int i1 = index;
    int i2 = i1 + 1 < count2 ? i1 + 1 : 0;
    ClipVertex* tClip = &c[0];
    t = &vertices2[i1];
    R = &xf2.R;
    tClip->v.x = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    tClip->v.y = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    id_setReferenceEdge(tClip->key, (uint32_t)edge1);
    id_setIncidentEdge(tClip->key, (uint32_t)i1);
    tClip = &c[1];
    t = &vertices2[i2];
    R = &xf2.R;
    tClip->v.x = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    tClip->v.y = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    id_setReferenceEdge(tClip->key, (uint32_t)edge1);
    id_setIncidentEdge(tClip->key, (uint32_t)i2);
    id_setIncidentVertex(tClip->key, 1);
}

static double FindMaxSeparation(int& edgeIndexOut, const Geom& poly1, const XForm& xf1, const Geom& poly2,
                                const XForm& xf2) {
    int count1 = poly1.vertexCount;
    const Vec2* normals1 = poly1.normals;
    const Mat22* R = &xf2.R;
    const Vec2* t = &poly2.centroid;
    double dX = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double dY = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    R = &xf1.R;
    t = &poly1.centroid;
    dX -= xf1.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    dY -= xf1.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    double dLocal1X = dX * xf1.R.col1.x + dY * xf1.R.col1.y;
    double dLocal1Y = dX * xf1.R.col2.x + dY * xf1.R.col2.y;
    int edge = 0;
    double maxDot = -NUM_MAX_VALUE;
    for (int i = 0; i < count1; ++i) {
        t = &normals1[i];
        double dot = t->x * dLocal1X + t->y * dLocal1Y;
        if (dot > maxDot) {
            maxDot = dot;
            edge = i;
        }
    }
    double s = EdgeSeparation(poly1, xf1, edge, poly2, xf2);
    if (s > 0) return s;
    int prevEdge = edge - 1 >= 0 ? edge - 1 : count1 - 1;
    double sPrev = EdgeSeparation(poly1, xf1, prevEdge, poly2, xf2);
    if (sPrev > 0) return sPrev;
    int nextEdge = edge + 1 < count1 ? edge + 1 : 0;
    double sNext = EdgeSeparation(poly1, xf1, nextEdge, poly2, xf2);
    if (sNext > 0) return sNext;
    int bestEdge, increment;
    double bestSeparation;
    if (sPrev > s && sPrev > sNext) {
        increment = -1;
        bestEdge = prevEdge;
        bestSeparation = sPrev;
    } else {
        if (sNext <= s) {
            edgeIndexOut = edge;
            return s;
        }
        increment = 1;
        bestEdge = nextEdge;
        bestSeparation = sNext;
    }
    // NB: the decompiled AS3 omits the bestEdge/bestSeparation update below;
    // the AVM2 p-code confirms it exists (setlocal 22 / setlocal 23).
    for (;;) {
        if (increment == -1)
            edge = bestEdge - 1 >= 0 ? bestEdge - 1 : count1 - 1;
        else
            edge = bestEdge + 1 < count1 ? bestEdge + 1 : 0;
        s = EdgeSeparation(poly1, xf1, edge, poly2, xf2);
        if (s > 0) return s;
        if (s > bestSeparation) {
            bestEdge = edge;
            bestSeparation = s;
        } else {
            break;
        }
    }
    edgeIndexOut = bestEdge;
    return bestSeparation;
}

static int ClipSegmentToLine(ClipVertex vOut[2], const ClipVertex vIn[2], const Vec2& normal, double offset) {
    int numOut = 0;
    const Vec2 vIn0 = vIn[0].v;
    const Vec2 vIn1 = vIn[1].v;
    double distance0 = b2Dot(normal, vIn0) - offset;
    double distance1 = b2Dot(normal, vIn1) - offset;
    if (distance0 <= 0) vOut[numOut++] = vIn[0];
    if (distance1 <= 0) vOut[numOut++] = vIn[1];
    if (distance0 * distance1 < 0) {
        double interp = distance0 / (distance0 - distance1);
        vOut[numOut].v.x = vIn0.x + interp * (vIn1.x - vIn0.x);
        vOut[numOut].v.y = vIn0.y + interp * (vIn1.y - vIn0.y);
        vOut[numOut].key = distance0 > 0 ? vIn[0].key : vIn[1].key;
        ++numOut;
    }
    return numOut;
}

void CollidePolygons(Manifold& m, const Geom& polyA, const XForm& xfA, const Geom& polyB, const XForm& xfB) {
    m.pointCount = 0;
    int edgeA = 0;
    double separationA = FindMaxSeparation(edgeA, polyA, xfA, polyB, xfB);
    if (separationA > 0) return;
    int edgeB = 0;
    double separationB = FindMaxSeparation(edgeB, polyB, xfB, polyA, xfA);
    if (separationB > 0) return;
    const Geom* poly1;
    const Geom* poly2;
    XForm xf1, xf2;
    int edge1;
    uint32_t flip;
    const double k_relativeTol = 0.98;
    const double k_absoluteTol = 0.001;
    if (separationB > k_relativeTol * separationA + k_absoluteTol) {
        poly1 = &polyB;
        poly2 = &polyA;
        xf1 = xfB;
        xf2 = xfA;
        edge1 = edgeB;
        flip = 1;
    } else {
        poly1 = &polyA;
        poly2 = &polyB;
        xf1 = xfA;
        xf2 = xfB;
        edge1 = edgeA;
        flip = 0;
    }
    ClipVertex incidentEdge[2];
    FindIncidentEdge(incidentEdge, *poly1, xf1, edge1, *poly2, xf2);
    int count1 = poly1->vertexCount;
    const Vec2* vertices1 = poly1->vertices;
    Vec2 v11 = vertices1[edge1];
    Vec2 v12 = edge1 + 1 < count1 ? vertices1[edge1 + 1] : vertices1[0];
    Vec2 sideNormal = b2MulMV(xf1.R, SubtractVV(v12, v11));
    sideNormal.Normalize();
    Vec2 frontNormal = b2CrossVF(sideNormal, 1);
    v11 = b2MulX(xf1, v11);
    v12 = b2MulX(xf1, v12);
    double frontOffset = b2Dot(frontNormal, v11);
    double sideOffset1 = -b2Dot(sideNormal, v11);
    double sideOffset2 = b2Dot(sideNormal, v12);
    ClipVertex clipPoints1[2];
    ClipVertex clipPoints2[2];
    if (ClipSegmentToLine(clipPoints1, incidentEdge, sideNormal.Negative(), sideOffset1) < 2) return;
    if (ClipSegmentToLine(clipPoints2, clipPoints1, sideNormal, sideOffset2) < 2) return;
    m.normal = flip ? frontNormal.Negative() : frontNormal;
    int pointCount = 0;
    for (int i = 0; i < settings::maxManifoldPoints; ++i) {
        const ClipVertex& cv = clipPoints2[i];
        double separation = b2Dot(frontNormal, cv.v) - frontOffset;
        if (separation <= 0) {
            ManifoldPoint& cp = m.points[pointCount];
            cp.separation = separation;
            cp.localPoint1 = b2MulXT(xfA, cv.v);
            cp.localPoint2 = b2MulXT(xfB, cv.v);
            cp.key = cv.key;
            id_setFlip(cp.key, flip);
            ++pointCount;
        }
    }
    m.pointCount = pointCount;
}

// ======================================================================
// GJK distance (b2Distance.as)
// ======================================================================

// Minimal "shape-like" adaptor: a polygon (core vertices) or a single point.
struct SupportShape {
    const Geom* poly = nullptr;  // null -> point
    Vec2 point;
    Vec2 GetFirstVertex(const XForm& xf) const {
        if (!poly) return point;
        return b2MulX(xf, poly->coreVertices[0]);
    }
    Vec2 Support(const XForm& xf, double dX, double dY) const {
        if (!poly) return point;
        const Mat22* R = &xf.R;
        double dLocalX = dX * R->col1.x + dY * R->col1.y;
        double dLocalY = dX * R->col2.x + dY * R->col2.y;
        int bestIndex = 0;
        const Vec2* t = &poly->coreVertices[0];
        double bestValue = t->x * dLocalX + t->y * dLocalY;
        for (int i = 1; i < poly->vertexCount; ++i) {
            t = &poly->coreVertices[i];
            double value = t->x * dLocalX + t->y * dLocalY;
            if (value > bestValue) {
                bestIndex = i;
                bestValue = value;
            }
        }
        R = &xf.R;
        t = &poly->coreVertices[bestIndex];
        return Vec2(xf.position.x + (R->col1.x * t->x + R->col2.x * t->y),
                    xf.position.y + (R->col1.y * t->x + R->col2.y * t->y));
    }
};

static int ProcessTwo(Vec2& x1, Vec2& x2, Vec2 p1s[3], Vec2 p2s[3], Vec2 points[3]) {
    Vec2& points_0 = points[0];
    Vec2& points_1 = points[1];
    Vec2& p1s_0 = p1s[0];
    Vec2& p1s_1 = p1s[1];
    Vec2& p2s_0 = p2s[0];
    Vec2& p2s_1 = p2s[1];
    double rX = -points_1.x;
    double rY = -points_1.y;
    double dX = points_0.x - points_1.x;
    double dY = points_0.y - points_1.y;
    double length = as3_sqrt(dX * dX + dY * dY);
    dX /= length;
    dY /= length;
    double lambda = rX * dX + rY * dY;
    if (lambda <= 0 || length < NUM_MIN_VALUE) {
        x1.SetV(p1s_1);
        x2.SetV(p2s_1);
        p1s_0.SetV(p1s_1);
        p2s_0.SetV(p2s_1);
        points_0.SetV(points_1);
        return 1;
    }
    lambda /= length;
    x1.x = p1s_1.x + lambda * (p1s_0.x - p1s_1.x);
    x1.y = p1s_1.y + lambda * (p1s_0.y - p1s_1.y);
    x2.x = p2s_1.x + lambda * (p2s_0.x - p2s_1.x);
    x2.y = p2s_1.y + lambda * (p2s_0.y - p2s_1.y);
    return 2;
}

static int ProcessThree(Vec2& x1, Vec2& x2, Vec2 p1s[3], Vec2 p2s[3], Vec2 points[3]) {
    Vec2& points_0 = points[0];
    Vec2& points_1 = points[1];
    Vec2& points_2 = points[2];
    Vec2& p1s_0 = p1s[0];
    Vec2& p1s_1 = p1s[1];
    Vec2& p1s_2 = p1s[2];
    Vec2& p2s_0 = p2s[0];
    Vec2& p2s_1 = p2s[1];
    Vec2& p2s_2 = p2s[2];
    double aX = points_0.x, aY = points_0.y;
    double bX = points_1.x, bY = points_1.y;
    double cX = points_2.x, cY = points_2.y;
    double abX = bX - aX, abY = bY - aY;
    double acX = cX - aX, acY = cY - aY;
    double bcX = cX - bX, bcY = cY - bY;
    double sn = -(aX * abX + aY * abY);
    double sd = bX * abX + bY * abY;
    double tn = -(aX * acX + aY * acY);
    double td = cX * acX + cY * acY;
    double un = -(bX * bcX + bY * bcY);
    double ud = cX * bcX + cY * bcY;
    (void)sn;
    (void)sd;
    if (td <= 0 && ud <= 0) {
        x1.SetV(p1s_2);
        x2.SetV(p2s_2);
        p1s_0.SetV(p1s_2);
        p2s_0.SetV(p2s_2);
        points_0.SetV(points_2);
        return 1;
    }
    double n = abX * acY - abY * acX;
    double vc = n * (aX * bY - aY * bX);
    double va = n * (bX * cY - bY * cX);
    if (va <= 0 && un >= 0 && ud >= 0 && un + ud > 0) {
        double lambda = un / (un + ud);
        x1.x = p1s_1.x + lambda * (p1s_2.x - p1s_1.x);
        x1.y = p1s_1.y + lambda * (p1s_2.y - p1s_1.y);
        x2.x = p2s_1.x + lambda * (p2s_2.x - p2s_1.x);
        x2.y = p2s_1.y + lambda * (p2s_2.y - p2s_1.y);
        p1s_0.SetV(p1s_2);
        p2s_0.SetV(p2s_2);
        points_0.SetV(points_2);
        return 2;
    }
    double vb = n * (cX * aY - cY * aX);
    if (vb <= 0 && tn >= 0 && td >= 0 && tn + td > 0) {
        double lambda = tn / (tn + td);
        x1.x = p1s_0.x + lambda * (p1s_2.x - p1s_0.x);
        x1.y = p1s_0.y + lambda * (p1s_2.y - p1s_0.y);
        x2.x = p2s_0.x + lambda * (p2s_2.x - p2s_0.x);
        x2.y = p2s_0.y + lambda * (p2s_2.y - p2s_0.y);
        p1s_1.SetV(p1s_2);
        p2s_1.SetV(p2s_2);
        points_1.SetV(points_2);
        return 2;
    }
    double denom = va + vb + vc;
    denom = 1 / denom;
    double u = va * denom;
    double v = vb * denom;
    double w = 1 - u - v;
    x1.x = u * p1s_0.x + v * p1s_1.x + w * p1s_2.x;
    x1.y = u * p1s_0.y + v * p1s_1.y + w * p1s_2.y;
    x2.x = u * p2s_0.x + v * p2s_1.x + w * p2s_2.x;
    x2.y = u * p2s_0.y + v * p2s_1.y + w * p2s_2.y;
    return 3;
}

static double DistanceGeneric(Vec2& x1, Vec2& x2, const SupportShape& shape1, const XForm& xf1,
                              const SupportShape& shape2, const XForm& xf2) {
    Vec2 p1s[3], p2s[3], points[3];
    int pointCount = 0;
    x1.SetV(shape1.GetFirstVertex(xf1));
    x2.SetV(shape2.GetFirstVertex(xf2));
    double vSqr = 0;
    const int maxIterations = 20;
    for (int iter = 0; iter < maxIterations; ++iter) {
        double vX = x2.x - x1.x;
        double vY = x2.y - x1.y;
        Vec2 w1 = shape1.Support(xf1, vX, vY);
        Vec2 w2 = shape2.Support(xf2, -vX, -vY);
        vSqr = vX * vX + vY * vY;
        double wX = w2.x - w1.x;
        double wY = w2.y - w1.y;
        double vw = vX * wX + vY * wY;
        if (vSqr - vw <= 0.01 * vSqr) {
            if (pointCount == 0) {
                x1.SetV(w1);
                x2.SetV(w2);
            }
            return as3_sqrt(vSqr);
        }
        switch (pointCount) {
            case 0:
                p1s[0].SetV(w1);
                p2s[0].SetV(w2);
                points[0].x = wX;
                points[0].y = wY;
                x1.SetV(p1s[0]);
                x2.SetV(p2s[0]);
                ++pointCount;
                break;
            case 1:
                p1s[1].SetV(w1);
                p2s[1].SetV(w2);
                points[1].x = wX;
                points[1].y = wY;
                pointCount = ProcessTwo(x1, x2, p1s, p2s, points);
                break;
            case 2:
                p1s[2].SetV(w1);
                p2s[2].SetV(w2);
                points[2].x = wX;
                points[2].y = wY;
                pointCount = ProcessThree(x1, x2, p1s, p2s, points);
                break;
        }
        if (pointCount == 3) return 0;
        double maxSqr = -NUM_MAX_VALUE;
        for (int i = 0; i < pointCount; ++i) {
            const Vec2& p = points[i];
            maxSqr = b2Max(maxSqr, p.x * p.x + p.y * p.y);
        }
        if (pointCount == 3 || vSqr <= 100 * NUM_MIN_VALUE * maxSqr) {
            vX = x2.x - x1.x;
            vY = x2.y - x1.y;
            vSqr = vX * vX + vY * vY;
            return as3_sqrt(vSqr);
        }
    }
    return as3_sqrt(vSqr);
}

static double DistanceCC(Vec2& x1, Vec2& x2, const Geom& circle1, const XForm& xf1, const Geom& circle2,
                         const XForm& xf2) {
    const Mat22* R = &xf1.R;
    const Vec2* t = &circle1.localPosition;
    double p1X = xf1.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double p1Y = xf1.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    R = &xf2.R;
    t = &circle2.localPosition;
    double p2X = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    double p2Y = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    double dX = p2X - p1X;
    double dY = p2Y - p1Y;
    double dSqr = dX * dX + dY * dY;
    double r1 = circle1.radius - settings::toiSlop;
    double r2 = circle2.radius - settings::toiSlop;
    double r = r1 + r2;
    if (dSqr > r * r) {
        double dLen = as3_sqrt(dSqr);
        dX /= dLen;
        dY /= dLen;
        double distance = dLen - r;
        x1.x = p1X + r1 * dX;
        x1.y = p1Y + r1 * dY;
        x2.x = p2X - r2 * dX;
        x2.y = p2Y - r2 * dY;
        return distance;
    }
    if (dSqr > NUM_MIN_VALUE * NUM_MIN_VALUE) {
        double dLen = as3_sqrt(dSqr);
        dX /= dLen;
        dY /= dLen;
        x1.x = p1X + r1 * dX;
        x1.y = p1Y + r1 * dY;
        x2.x = x1.x;
        x2.y = x1.y;
        return 0;
    }
    x1.x = p1X;
    x1.y = p1Y;
    x2.x = x1.x;
    x2.y = x1.y;
    return 0;
}

static double DistancePC(Vec2& x1, Vec2& x2, const Geom& polygon, const XForm& xf1, const Geom& circle,
                         const XForm& xf2) {
    SupportShape point;
    const Vec2* t = &circle.localPosition;
    const Mat22* R = &xf2.R;
    point.point.x = xf2.position.x + (R->col1.x * t->x + R->col2.x * t->y);
    point.point.y = xf2.position.y + (R->col1.y * t->x + R->col2.y * t->y);
    SupportShape poly;
    poly.poly = &polygon;
    XForm identity;
    identity.SetIdentity();  // b2Math.b2XForm_identity
    double distance = DistanceGeneric(x1, x2, poly, xf1, point, identity);
    double r = circle.radius - settings::toiSlop;
    if (distance > r) {
        distance -= r;
        double dX = x2.x - x1.x;
        double dY = x2.y - x1.y;
        double dLen = as3_sqrt(dX * dX + dY * dY);
        dX /= dLen;
        dY /= dLen;
        x2.x -= r * dX;
        x2.y -= r * dY;
    } else {
        distance = 0;
        x2.x = x1.x;
        x2.y = x1.y;
    }
    return distance;
}

static double Distance(Vec2& x1, Vec2& x2, const Geom& g1, const XForm& xf1, const Geom& g2, const XForm& xf2) {
    if (g1.type == e_circleShape && g2.type == e_circleShape) return DistanceCC(x1, x2, g1, xf1, g2, xf2);
    if (g1.type == e_polygonShape && g2.type == e_circleShape) return DistancePC(x1, x2, g1, xf1, g2, xf2);
    if (g1.type == e_circleShape && g2.type == e_polygonShape) return DistancePC(x2, x1, g2, xf2, g1, xf1);
    if (g1.type == e_polygonShape && g2.type == e_polygonShape) {
        SupportShape a, b;
        a.poly = &g1;
        b.poly = &g2;
        return DistanceGeneric(x1, x2, a, xf1, b, xf2);
    }
    return 0;
}

// ======================================================================
// Time of impact (b2TimeOfImpact.as)
// ======================================================================

double TimeOfImpact(const Geom& g1, const Shape& s1, const Sweep& sweep1, const Geom& g2, const Shape& s2,
                    const Sweep& sweep2) {
    double r1 = s1.sweepRadius;
    double r2 = s2.sweepRadius;
    double t0 = sweep1.t0;
    double v1X = sweep1.c.x - sweep1.c0.x;
    double v1Y = sweep1.c.y - sweep1.c0.y;
    double v2X = sweep2.c.x - sweep2.c0.x;
    double v2Y = sweep2.c.y - sweep2.c0.y;
    double omega1 = sweep1.a - sweep1.a0;
    double omega2 = sweep2.a - sweep2.a0;
    double alpha = 0;
    Vec2 p1, p2;
    const int k_maxIterations = 20;
    int iter = 0;
    double distance = 0;
    double targetDistance = 0;
    for (;;) {
        double t = (1 - alpha) * t0 + alpha;
        XForm xf1, xf2;
        sweep1.GetXForm(xf1, t);
        sweep2.GetXForm(xf2, t);
        distance = Distance(p1, p2, g1, xf1, g2, xf2);
        if (iter == 0) {
            if (distance > 2 * settings::toiSlop) {
                targetDistance = 1.5 * settings::toiSlop;
            } else {
                double a = 0.05 * settings::toiSlop;
                double b = distance - 0.5 * settings::toiSlop;
                targetDistance = a > b ? a : b;
            }
        }
        if (distance - targetDistance < 0.05 * settings::toiSlop || iter == k_maxIterations) break;
        double nX = p2.x - p1.x;
        double nY = p2.y - p1.y;
        double nLen = as3_sqrt(nX * nX + nY * nY);
        nX /= nLen;
        nY /= nLen;
        double approachVelocityBound = nX * (v1X - v2X) + nY * (v1Y - v2Y) +
                                       (omega1 < 0 ? -omega1 : omega1) * r1 +
                                       (omega2 < 0 ? -omega2 : omega2) * r2;
        if (approachVelocityBound == 0) {
            alpha = 1;
            break;
        }
        double dAlpha = (distance - targetDistance) / approachVelocityBound;
        double newAlpha = alpha + dAlpha;
        if (newAlpha < 0 || 1 < newAlpha) {
            alpha = 1;
            break;
        }
        if (newAlpha < (1 + 100 * NUM_MIN_VALUE) * alpha) break;
        alpha = newAlpha;
        ++iter;
    }
    return alpha;
}

}  // namespace rb
