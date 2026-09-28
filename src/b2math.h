// b2math.h - math primitives transliterated from the Box2DFlash (AS3) build
// embedded in Red Ball 1. Every expression keeps the AS3 operand order so that
// IEEE-754 double results match the Flash VM bit-for-bit.
//
// Compile with -ffp-contract=off (no FMA fusion) and without -ffast-math.
#pragma once
#include <cmath>
#include <cstdint>
#include <cfloat>
#include <limits>

namespace rb {

// AS3 Number constants
constexpr double NUM_MIN_VALUE = 4.9406564584124654e-324;  // Number.MIN_VALUE (denormal!)
constexpr double NUM_MAX_VALUE = DBL_MAX;                   // Number.MAX_VALUE
constexpr double NUM_NAN = std::numeric_limits<double>::quiet_NaN();
constexpr double AS3_PI = 3.141592653589793;                // Math.PI

// Single choke point for transcendental functions. Flash's Math.sin/cos may
// not be bit-identical to glibc; if verification shows drift on rotating
// bodies, replace these two functions only.
// Optional override hook (used by diagnostics; null in normal runs).
extern double (*g_sinHook)(double);
extern double (*g_cosHook)(double);
inline double as3_sin(double a) { return __builtin_expect(g_sinHook != nullptr, 0) ? g_sinHook(a) : std::sin(a); }
inline double as3_cos(double a) { return __builtin_expect(g_cosHook != nullptr, 0) ? g_cosHook(a) : std::cos(a); }
inline double as3_sqrt(double a) { return std::sqrt(a); }  // IEEE correctly rounded everywhere

// ECMAScript ToUint32 / ToInt32
inline uint32_t as3_toUint32(double v) {
    if (!std::isfinite(v)) return 0;
    double t = std::trunc(v);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}
inline int32_t as3_toInt32(double v) { return (int32_t)as3_toUint32(v); }

// b2Settings
namespace settings {
constexpr double angularSleepTolerance = 2.0 / 180.0;
constexpr double linearSleepTolerance = 0.01;
constexpr double linearSlop = 0.005;
constexpr double pi = AS3_PI;
constexpr double angularSlop = 2.0 / 180.0 * pi;
constexpr int maxProxies = 512;           // semantic value (only bounds/limits use it)
constexpr int maxPolygonVertices = 20;
constexpr double velocityThreshold = 1;
constexpr double contactBaumgarte = 0.2;
constexpr int maxPairs = 8 * maxProxies;
constexpr int maxTOIContactsPerIsland = 32;
constexpr double timeToSleep = 0.5;
constexpr int maxManifoldPoints = 2;
constexpr double maxAngularVelocity = 250;
constexpr double maxAngularVelocitySquared = maxAngularVelocity * maxAngularVelocity;
constexpr double maxAngularCorrection = 8.0 / 180.0 * pi;
constexpr uint32_t USHRT_MAX_ = 65535;
constexpr double maxLinearVelocity = 200;
constexpr double maxLinearCorrection = 0.2;
constexpr double toiSlop = 8 * linearSlop;
constexpr double maxLinearVelocitySquared = maxLinearVelocity * maxLinearVelocity;
}  // namespace settings

// b2Math.b2Min / b2Max / b2Clamp keep the exact ternary semantics (NaN behaviour)
inline double b2Min(double a, double b) { return a < b ? a : b; }
inline double b2Max(double a, double b) { return a > b ? a : b; }
inline double b2Clamp(double a, double lo, double hi) { return b2Max(lo, b2Min(a, hi)); }

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    void Set(double x_, double y_) { x = x_; y = y_; }
    void SetV(const Vec2& v) { x = v.x; y = v.y; }
    void SetZero() { x = 0; y = 0; }
    double Length() const { return as3_sqrt(x * x + y * y); }
    double LengthSquared() const { return x * x + y * y; }
    Vec2 Negative() const { return Vec2(-x, -y); }
    void Multiply(double a) { x *= a; y *= a; }
    // b2Vec2.Normalize: note Number.MIN_VALUE threshold
    double Normalize() {
        double len = as3_sqrt(x * x + y * y);
        if (len < NUM_MIN_VALUE) return 0;
        double inv = 1 / len;
        x *= inv;
        y *= inv;
        return len;
    }
    void Abs() {
        if (x < 0) x = -x;
        if (y < 0) y = -y;
    }
};

struct Mat22 {
    Vec2 col1, col2;
    // b2Mat22.Set(angle)
    void Set(double angle) {
        double c = as3_cos(angle);
        double s = as3_sin(angle);
        col1.x = c;
        col2.x = -s;
        col1.y = s;
        col2.y = c;
    }
    void SetIdentity() { col1.x = 1; col2.x = 0; col1.y = 0; col2.y = 1; }
    void SetZero() { col1.x = 0; col2.x = 0; col1.y = 0; col2.y = 0; }
    void Abs() { col1.Abs(); col2.Abs(); }
    // b2Mat22.Invert(out)
    void Invert(Mat22& out) const {
        double a = col1.x, b = col2.x, c = col1.y, d = col2.y;
        double det = a * d - b * c;
        det = 1 / det;
        out.col1.x = det * d;
        out.col2.x = -det * b;
        out.col1.y = -det * c;
        out.col2.y = det * a;
    }
    // b2Mat22.Solve(out, bX, bY)
    Vec2 Solve(double bX, double bY) const {
        double a11 = col1.x, a12 = col2.x, a21 = col1.y, a22 = col2.y;
        double det = a11 * a22 - a12 * a21;
        det = 1 / det;
        return Vec2(det * (a22 * bX - a12 * bY), det * (a11 * bY - a21 * bX));
    }
};

struct XForm {
    Vec2 position;
    Mat22 R;
    void SetIdentity() { position.SetZero(); R.SetIdentity(); }
};

// b2Math helpers (exact expression forms)
inline Vec2 b2MulMV(const Mat22& A, const Vec2& v) {
    return Vec2(A.col1.x * v.x + A.col2.x * v.y, A.col1.y * v.x + A.col2.y * v.y);
}
inline Vec2 b2MulX(const XForm& T, const Vec2& v) {
    Vec2 r = b2MulMV(T.R, v);
    r.x += T.position.x;
    r.y += T.position.y;
    return r;
}
inline Vec2 b2MulXT(const XForm& T, const Vec2& v) {
    Vec2 r(v.x - T.position.x, v.y - T.position.y);
    double tX = r.x * T.R.col1.x + r.y * T.R.col1.y;
    r.y = r.x * T.R.col2.x + r.y * T.R.col2.y;
    r.x = tX;
    return r;
}
inline double b2Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline double b2CrossVV(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline Vec2 b2CrossVF(const Vec2& a, double s) { return Vec2(s * a.y, -s * a.x); }
inline Vec2 SubtractVV(const Vec2& a, const Vec2& b) { return Vec2(a.x - b.x, a.y - b.y); }

struct Sweep {
    Vec2 localCenter;
    double a = NUM_NAN;
    Vec2 c;
    double a0 = NUM_NAN;
    Vec2 c0;
    double t0 = NUM_NAN;

    void Advance(double t) {
        if (t0 < t && 1 - t0 > NUM_MIN_VALUE) {
            double alpha = (t - t0) / (1 - t0);
            c0.x = (1 - alpha) * c0.x + alpha * c.x;
            c0.y = (1 - alpha) * c0.y + alpha * c.y;
            a0 = (1 - alpha) * a0 + alpha * a;
            t0 = t;
        }
    }
    void GetXForm(XForm& xf, double t) const {
        if (1 - t0 > NUM_MIN_VALUE) {
            double alpha = (t - t0) / (1 - t0);
            xf.position.x = (1 - alpha) * c0.x + alpha * c.x;
            xf.position.y = (1 - alpha) * c0.y + alpha * c.y;
            double angle = (1 - alpha) * a0 + alpha * a;
            xf.R.Set(angle);
        } else {
            xf.position.SetV(c);
            xf.R.Set(a);
        }
        const Mat22& R = xf.R;
        xf.position.x -= R.col1.x * localCenter.x + R.col2.x * localCenter.y;
        xf.position.y -= R.col1.y * localCenter.x + R.col2.y * localCenter.y;
    }
};

struct AABB {
    Vec2 lowerBound, upperBound;
    bool IsValid() const {
        double dx = upperBound.x - lowerBound.x;
        double dy = upperBound.y - lowerBound.y;
        bool valid = dx >= 0 && dy >= 0;
        return valid && std::isfinite(lowerBound.x) && std::isfinite(lowerBound.y) &&
               std::isfinite(upperBound.x) && std::isfinite(upperBound.y);
    }
};

}  // namespace rb
