// b2joints.cpp - b2DistanceJoint / b2PrismaticJoint / b2RevoluteJoint and
// b2World.CreateJoint / DestroyJoint, transliterated from the Red Ball SWF
// (Box2DFlash 2.0.x, force-based joint formulation). Every method's field
// writes were cross-checked against the AVM2 p-code (no decompiler losses).
#include "b2world.h"

namespace rb {

static inline JointEdge& JE(World& w, int32_t ref) { return w.joints[ref >> 1].node[ref & 1]; }
static inline double b2Abs(double a) { return a > 0 ? a : -a; }

// local anchor -> world-relative lever arm r = R * (localAnchor - localCenter)
static inline void LeverArm(const Body& b, const Vec2& localAnchor, double& rX, double& rY) {
    const Mat22& R = b.xf.R;
    rX = localAnchor.x - b.sweep.localCenter.x;
    rY = localAnchor.y - b.sweep.localCenter.y;
    double t = R.col1.x * rX + R.col2.x * rY;
    rY = R.col1.y * rX + R.col2.y * rY;
    rX = t;
}

// ================================================================ defs

void World::InitDistanceJointDef(JointDef& d, int32_t b1, int32_t b2, const Vec2& anchor1,
                                 const Vec2& anchor2) const {
    d.type = JT_DISTANCE;
    d.body1 = b1;
    d.body2 = b2;
    d.localAnchor1 = GetLocalPoint(b1, anchor1);
    d.localAnchor2 = GetLocalPoint(b2, anchor2);
    double dX = anchor2.x - anchor1.x;
    double dY = anchor2.y - anchor1.y;
    d.length = as3_sqrt(dX * dX + dY * dY);
    d.frequencyHz = 0;
    d.dampingRatio = 0;
}

void World::InitPrismaticJointDef(JointDef& d, int32_t b1, int32_t b2, const Vec2& anchor, const Vec2& axis) const {
    d.type = JT_PRISMATIC;
    d.body1 = b1;
    d.body2 = b2;
    d.localAnchor1 = GetLocalPoint(b1, anchor);
    d.localAnchor2 = GetLocalPoint(b2, anchor);
    d.localAxis1 = GetLocalVector(b1, axis);
    d.referenceAngle = bodies[b2].sweep.a - bodies[b1].sweep.a;
}

void World::InitRevoluteJointDef(JointDef& d, int32_t b1, int32_t b2, const Vec2& anchor) const {
    d.type = JT_REVOLUTE;
    d.body1 = b1;
    d.body2 = b2;
    d.localAnchor1 = GetLocalPoint(b1, anchor);
    d.localAnchor2 = GetLocalPoint(b2, anchor);
    d.referenceAngle = bodies[b2].sweep.a - bodies[b1].sweep.a;
}

// ================================================================ world

int32_t World::CreateJoint(const JointDef& def) {
    if (def.type != JT_DISTANCE && def.type != JT_PRISMATIC && def.type != JT_REVOLUTE)
        fatal("joint type not implemented (only distance/prismatic/revolute are used by the game)");
    if (numJoints >= CAP_JOINTS) fatal("joint capacity exceeded");
    int32_t j = numJoints++;
    Joint& jt = joints[j];
    jt = Joint();
    jt.type = def.type;
    jt.prev = -1;
    jt.next = -1;
    jt.body1 = def.body1;
    jt.body2 = def.body2;
    jt.collideConnected = def.collideConnected;
    jt.islandFlag = false;
    jt.alive = true;
    switch (def.type) {
        case JT_DISTANCE:
            jt.localAnchor1 = def.localAnchor1;
            jt.localAnchor2 = def.localAnchor2;
            jt.length = def.length;
            jt.frequencyHz = def.frequencyHz;
            jt.dampingRatio = def.dampingRatio;
            jt.impulse = 0;
            jt.gamma = 0;
            jt.bias = 0;
            jt.inv_dt = 0;
            break;
        case JT_PRISMATIC:
            jt.localAnchor1 = def.localAnchor1;
            jt.localAnchor2 = def.localAnchor2;
            jt.localXAxis1 = def.localAxis1;
            jt.localYAxis1.x = -jt.localXAxis1.y;
            jt.localYAxis1.y = jt.localXAxis1.x;
            jt.refAngle = def.referenceAngle;
            jt.linearJacobian.SetZero();
            jt.linearMass = 0;
            jt.force = 0;
            jt.angularMass = 0;
            jt.torque = 0;
            jt.motorJacobian.SetZero();
            jt.motorMass = 0;
            jt.motorForce = 0;
            jt.limitForce = 0;
            jt.limitPositionImpulse = 0;
            jt.lowerTranslation = def.lowerTranslation;
            jt.upperTranslation = def.upperTranslation;
            jt.maxMotorForce = def.maxMotorForce;
            jt.motorSpeed = def.motorSpeed;
            jt.enableLimit = def.enableLimit;
            jt.enableMotor = def.enableMotor;
            break;
        case JT_REVOLUTE:
            jt.localAnchor1 = def.localAnchor1;
            jt.localAnchor2 = def.localAnchor2;
            jt.referenceAngle = def.referenceAngle;
            jt.pivotForce.Set(0, 0);
            jt.motorForce = 0;
            jt.limitForce = 0;
            jt.limitPositionImpulse = 0;
            jt.lowerAngle = def.lowerAngle;
            jt.upperAngle = def.upperAngle;
            jt.maxMotorTorque = def.maxMotorTorque;
            jt.motorSpeed = def.motorSpeed;
            jt.enableLimit = def.enableLimit;
            jt.enableMotor = def.enableMotor;
            break;
    }
    // link into the world list
    jt.prev = -1;
    jt.next = jointList;
    if (jointList != -1) joints[jointList].prev = j;
    jointList = j;
    ++jointCount;
    // body edges
    const int32_t e1 = j * 2 + 0, e2 = j * 2 + 1;
    Body& b1 = bodies[jt.body1];
    Body& b2 = bodies[jt.body2];
    jt.node[0].other = jt.body2;
    jt.node[0].prev = -1;
    jt.node[0].next = b1.jointList;
    if (b1.jointList != -1) JE(*this, b1.jointList).prev = e1;
    b1.jointList = e1;
    jt.node[1].other = jt.body1;
    jt.node[1].prev = -1;
    jt.node[1].next = b2.jointList;
    if (b2.jointList != -1) JE(*this, b2.jointList).prev = e2;
    b2.jointList = e2;
    if (def.collideConnected == false) {
        int32_t b = bodies[def.body1].shapeCount < bodies[def.body2].shapeCount ? def.body1 : def.body2;
        for (int32_t s = bodies[b].shapeList; s != -1; s = shapes[s].next) Shape_RefilterProxy(s, bodies[b].xf);
    }
    return j;
}

void World::DestroyJoint(int32_t j) {
    Joint& jt = joints[j];
    bool collideConnected = jt.collideConnected;
    if (jt.prev != -1) joints[jt.prev].next = jt.next;
    if (jt.next != -1) joints[jt.next].prev = jt.prev;
    if (j == jointList) jointList = jt.next;
    Body& b1 = bodies[jt.body1];
    Body& b2 = bodies[jt.body2];
    b1.WakeUp();
    b2.WakeUp();
    const int32_t e1 = j * 2 + 0, e2 = j * 2 + 1;
    if (jt.node[0].prev != -1) JE(*this, jt.node[0].prev).next = jt.node[0].next;
    if (jt.node[0].next != -1) JE(*this, jt.node[0].next).prev = jt.node[0].prev;
    if (e1 == b1.jointList) b1.jointList = jt.node[0].next;
    jt.node[0].prev = jt.node[0].next = -1;
    if (jt.node[1].prev != -1) JE(*this, jt.node[1].prev).next = jt.node[1].next;
    if (jt.node[1].next != -1) JE(*this, jt.node[1].next).prev = jt.node[1].prev;
    if (e2 == b2.jointList) b2.jointList = jt.node[1].next;
    jt.node[1].prev = jt.node[1].next = -1;
    --jointCount;
    jt.alive = false;
    if (collideConnected == false) {
        int32_t b = b1.shapeCount < b2.shapeCount ? jt.body1 : jt.body2;
        for (int32_t s = bodies[b].shapeList; s != -1; s = shapes[s].next) Shape_RefilterProxy(s, bodies[b].xf);
    }
}

// ================================================================ distance

static void DistanceInitVelocity(Joint& J, Body& b1, Body& b2, const TimeStep& step) {
    J.inv_dt = step.inv_dt;
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    J.u.x = b2.sweep.c.x + r2X - b1.sweep.c.x - r1X;
    J.u.y = b2.sweep.c.y + r2Y - b1.sweep.c.y - r1Y;
    double length = as3_sqrt(J.u.x * J.u.x + J.u.y * J.u.y);
    if (length > settings::linearSlop)
        J.u.Multiply(1 / length);
    else
        J.u.SetZero();
    double cr1u = r1X * J.u.y - r1Y * J.u.x;
    double cr2u = r2X * J.u.y - r2Y * J.u.x;
    double invMass = b1.invMass + b1.invI * cr1u * cr1u + b2.invMass + b2.invI * cr2u * cr2u;
    J.mass = 1 / invMass;
    if (J.frequencyHz > 0) {
        double C = length - J.length;
        double omega = 2 * AS3_PI * J.frequencyHz;
        double d = 2 * J.mass * J.dampingRatio * omega;
        double k = J.mass * omega * omega;
        J.gamma = 1 / (step.dt * (d + step.dt * k));
        J.bias = C * step.dt * k * J.gamma;
        J.mass = 1 / (invMass + J.gamma);
    }
    if (step.warmStarting) {
        J.impulse *= step.dtRatio;
        double PX = J.impulse * J.u.x;
        double PY = J.impulse * J.u.y;
        b1.linearVelocity.x -= b1.invMass * PX;
        b1.linearVelocity.y -= b1.invMass * PY;
        b1.angularVelocity -= b1.invI * (r1X * PY - r1Y * PX);
        b2.linearVelocity.x += b2.invMass * PX;
        b2.linearVelocity.y += b2.invMass * PY;
        b2.angularVelocity += b2.invI * (r2X * PY - r2Y * PX);
    } else {
        J.impulse = 0;
    }
}

static void DistanceSolveVelocity(Joint& J, Body& b1, Body& b2) {
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double v1X = b1.linearVelocity.x + -b1.angularVelocity * r1Y;
    double v1Y = b1.linearVelocity.y + b1.angularVelocity * r1X;
    double v2X = b2.linearVelocity.x + -b2.angularVelocity * r2Y;
    double v2Y = b2.linearVelocity.y + b2.angularVelocity * r2X;
    double Cdot = J.u.x * (v2X - v1X) + J.u.y * (v2Y - v1Y);
    double impulse = -J.mass * (Cdot + J.bias + J.gamma * J.impulse);
    J.impulse += impulse;
    double PX = impulse * J.u.x;
    double PY = impulse * J.u.y;
    b1.linearVelocity.x -= b1.invMass * PX;
    b1.linearVelocity.y -= b1.invMass * PY;
    b1.angularVelocity -= b1.invI * (r1X * PY - r1Y * PX);
    b2.linearVelocity.x += b2.invMass * PX;
    b2.linearVelocity.y += b2.invMass * PY;
    b2.angularVelocity += b2.invI * (r2X * PY - r2Y * PX);
}

static bool DistanceSolvePosition(World& w, Joint& J, int32_t b1i, int32_t b2i) {
    if (J.frequencyHz > 0) return true;
    Body& b1 = w.bodies[b1i];
    Body& b2 = w.bodies[b2i];
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double dX = b2.sweep.c.x + r2X - b1.sweep.c.x - r1X;
    double dY = b2.sweep.c.y + r2Y - b1.sweep.c.y - r1Y;
    double length = as3_sqrt(dX * dX + dY * dY);
    dX /= length;
    dY /= length;
    double C = length - J.length;
    C = b2Clamp(C, -settings::maxLinearCorrection, settings::maxLinearCorrection);
    double impulse = -J.mass * C;
    J.u.Set(dX, dY);
    double PX = impulse * J.u.x;
    double PY = impulse * J.u.y;
    b1.sweep.c.x -= b1.invMass * PX;
    b1.sweep.c.y -= b1.invMass * PY;
    b1.sweep.a -= b1.invI * (r1X * PY - r1Y * PX);
    b2.sweep.c.x += b2.invMass * PX;
    b2.sweep.c.y += b2.invMass * PY;
    b2.sweep.a += b2.invI * (r2X * PY - r2Y * PX);
    w.SynchronizeTransform(b1i);
    w.SynchronizeTransform(b2i);
    return b2Abs(C) < settings::linearSlop;
}

// ================================================================ prismatic

static void PrismaticInitVelocity(Joint& J, Body& b1, Body& b2, const TimeStep& step) {
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double invMass1 = b1.invMass, invMass2 = b2.invMass;
    double invI1 = b1.invI, invI2 = b2.invI;
    const Mat22& R1 = b1.xf.R;
    double ay1X = R1.col1.x * J.localYAxis1.x + R1.col2.x * J.localYAxis1.y;
    double ay1Y = R1.col1.y * J.localYAxis1.x + R1.col2.y * J.localYAxis1.y;
    double eX = b2.sweep.c.x + r2X - b1.sweep.c.x;
    double eY = b2.sweep.c.y + r2Y - b1.sweep.c.y;
    Jacobian& LJ = J.linearJacobian;
    LJ.linear1.x = -ay1X;
    LJ.linear1.y = -ay1Y;
    LJ.linear2.x = ay1X;
    LJ.linear2.y = ay1Y;
    LJ.angular1 = -(eX * ay1Y - eY * ay1X);
    LJ.angular2 = r2X * ay1Y - r2Y * ay1X;
    J.linearMass = invMass1 + invI1 * LJ.angular1 * LJ.angular1 + invMass2 + invI2 * LJ.angular2 * LJ.angular2;
    J.linearMass = 1 / J.linearMass;
    J.angularMass = invI1 + invI2;
    if (J.angularMass > NUM_MIN_VALUE) J.angularMass = 1 / J.angularMass;
    if (J.enableLimit || J.enableMotor) {
        double ax1X = R1.col1.x * J.localXAxis1.x + R1.col2.x * J.localXAxis1.y;
        double ax1Y = R1.col1.y * J.localXAxis1.x + R1.col2.y * J.localXAxis1.y;
        Jacobian& MJ = J.motorJacobian;
        MJ.linear1.x = -ax1X;
        MJ.linear1.y = -ax1Y;
        MJ.linear2.x = ax1X;
        MJ.linear2.y = ax1Y;
        MJ.angular1 = -(eX * ax1Y - eY * ax1X);
        MJ.angular2 = r2X * ax1Y - r2Y * ax1X;
        J.motorMass = invMass1 + invI1 * MJ.angular1 * MJ.angular1 + invMass2 + invI2 * MJ.angular2 * MJ.angular2;
        J.motorMass = 1 / J.motorMass;
        if (J.enableLimit) {
            double dX = eX - r1X;
            double dY = eY - r1Y;
            double jointTranslation = ax1X * dX + ax1Y * dY;
            if (b2Abs(J.upperTranslation - J.lowerTranslation) < 2 * settings::linearSlop) {
                J.limitState = LS_EQUAL;
            } else if (jointTranslation <= J.lowerTranslation) {
                if (J.limitState != LS_AT_LOWER) J.limitForce = 0;
                J.limitState = LS_AT_LOWER;
            } else if (jointTranslation >= J.upperTranslation) {
                if (J.limitState != LS_AT_UPPER) J.limitForce = 0;
                J.limitState = LS_AT_UPPER;
            } else {
                J.limitState = LS_INACTIVE;
                J.limitForce = 0;
            }
        }
    }
    if (J.enableMotor == false) J.motorForce = 0;
    if (J.enableLimit == false) J.limitForce = 0;
    if (step.warmStarting) {
        const Jacobian& MJ = J.motorJacobian;
        double P1X = step.dt * (J.force * LJ.linear1.x + (J.motorForce + J.limitForce) * MJ.linear1.x);
        double P1Y = step.dt * (J.force * LJ.linear1.y + (J.motorForce + J.limitForce) * MJ.linear1.y);
        double P2X = step.dt * (J.force * LJ.linear2.x + (J.motorForce + J.limitForce) * MJ.linear2.x);
        double P2Y = step.dt * (J.force * LJ.linear2.y + (J.motorForce + J.limitForce) * MJ.linear2.y);
        double L1 = step.dt * (J.force * LJ.angular1 - J.torque + (J.motorForce + J.limitForce) * MJ.angular1);
        double L2 = step.dt * (J.force * LJ.angular2 + J.torque + (J.motorForce + J.limitForce) * MJ.angular2);
        b1.linearVelocity.x += invMass1 * P1X;
        b1.linearVelocity.y += invMass1 * P1Y;
        b1.angularVelocity += invI1 * L1;
        b2.linearVelocity.x += invMass2 * P2X;
        b2.linearVelocity.y += invMass2 * P2Y;
        b2.angularVelocity += invI2 * L2;
    } else {
        J.force = 0;
        J.torque = 0;
        J.limitForce = 0;
        J.motorForce = 0;
    }
    J.limitPositionImpulse = 0;
}

static inline void ApplyJacobian(Body& b1, Body& b2, const Jacobian& JJ, double P) {
    b1.linearVelocity.x += b1.invMass * P * JJ.linear1.x;
    b1.linearVelocity.y += b1.invMass * P * JJ.linear1.y;
    b1.angularVelocity += b1.invI * P * JJ.angular1;
    b2.linearVelocity.x += b2.invMass * P * JJ.linear2.x;
    b2.linearVelocity.y += b2.invMass * P * JJ.linear2.y;
    b2.angularVelocity += b2.invI * P * JJ.angular2;
}

static void PrismaticSolveVelocity(Joint& J, Body& b1, Body& b2, const TimeStep& step) {
    double invI1 = b1.invI, invI2 = b2.invI;
    double linearCdot = J.linearJacobian.Compute(b1.linearVelocity, b1.angularVelocity, b2.linearVelocity,
                                                 b2.angularVelocity);
    double force = -step.inv_dt * J.linearMass * linearCdot;
    J.force += force;
    double P = step.dt * force;
    ApplyJacobian(b1, b2, J.linearJacobian, P);
    double angularCdot = b2.angularVelocity - b1.angularVelocity;
    double torque = -step.inv_dt * J.angularMass * angularCdot;
    J.torque += torque;
    double L = step.dt * torque;
    b1.angularVelocity -= invI1 * L;
    b2.angularVelocity += invI2 * L;
    if (J.enableMotor && J.limitState != LS_EQUAL) {
        double motorCdot = J.motorJacobian.Compute(b1.linearVelocity, b1.angularVelocity, b2.linearVelocity,
                                                   b2.angularVelocity) -
                           J.motorSpeed;
        double motorForce = -step.inv_dt * J.motorMass * motorCdot;
        double oldMotorForce = J.motorForce;
        J.motorForce = b2Clamp(J.motorForce + motorForce, -J.maxMotorForce, J.maxMotorForce);
        motorForce = J.motorForce - oldMotorForce;
        P = step.dt * motorForce;
        ApplyJacobian(b1, b2, J.motorJacobian, P);
    }
    if (J.enableLimit && J.limitState != LS_INACTIVE) {
        double limitCdot = J.motorJacobian.Compute(b1.linearVelocity, b1.angularVelocity, b2.linearVelocity,
                                                   b2.angularVelocity);
        double limitForce = -step.inv_dt * J.motorMass * limitCdot;
        if (J.limitState == LS_EQUAL) {
            J.limitForce += limitForce;
        } else if (J.limitState == LS_AT_LOWER) {
            double old = J.limitForce;
            J.limitForce = b2Max(J.limitForce + limitForce, 0);
            limitForce = J.limitForce - old;
        } else if (J.limitState == LS_AT_UPPER) {
            double old = J.limitForce;
            J.limitForce = b2Min(J.limitForce + limitForce, 0);
            limitForce = J.limitForce - old;
        }
        P = step.dt * limitForce;
        ApplyJacobian(b1, b2, J.motorJacobian, P);
    }
}

static inline void ApplyJacobianPos(Body& b1, Body& b2, const Jacobian& JJ, double invMass1, double invMass2,
                                    double invI1, double invI2, double impulse) {
    b1.sweep.c.x += invMass1 * impulse * JJ.linear1.x;
    b1.sweep.c.y += invMass1 * impulse * JJ.linear1.y;
    b1.sweep.a += invI1 * impulse * JJ.angular1;
    b2.sweep.c.x += invMass2 * impulse * JJ.linear2.x;
    b2.sweep.c.y += invMass2 * impulse * JJ.linear2.y;
    b2.sweep.a += invI2 * impulse * JJ.angular2;
}

static bool PrismaticSolvePosition(World& w, Joint& J, int32_t b1i, int32_t b2i) {
    Body& b1 = w.bodies[b1i];
    Body& b2 = w.bodies[b2i];
    double invMass1 = b1.invMass, invMass2 = b2.invMass;
    double invI1 = b1.invI, invI2 = b2.invI;
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double p1X = b1.sweep.c.x + r1X;
    double p1Y = b1.sweep.c.y + r1Y;
    double p2X = b2.sweep.c.x + r2X;
    double p2Y = b2.sweep.c.y + r2Y;
    double dX = p2X - p1X;
    double dY = p2Y - p1Y;
    const Mat22* R = &b1.xf.R;
    double ay1X = R->col1.x * J.localYAxis1.x + R->col2.x * J.localYAxis1.y;
    double ay1Y = R->col1.y * J.localYAxis1.x + R->col2.y * J.localYAxis1.y;
    double linearC = ay1X * dX + ay1Y * dY;
    linearC = b2Clamp(linearC, -settings::maxLinearCorrection, settings::maxLinearCorrection);
    double linearImpulse = -J.linearMass * linearC;
    ApplyJacobianPos(b1, b2, J.linearJacobian, invMass1, invMass2, invI1, invI2, linearImpulse);
    double positionError = b2Abs(linearC);
    double angularC = b2.sweep.a - b1.sweep.a - J.refAngle;
    angularC = b2Clamp(angularC, -settings::maxAngularCorrection, settings::maxAngularCorrection);
    double angularImpulse = -J.angularMass * angularC;
    b1.sweep.a -= b1.invI * angularImpulse;
    b2.sweep.a += b2.invI * angularImpulse;
    w.SynchronizeTransform(b1i);
    w.SynchronizeTransform(b2i);
    double angularError = b2Abs(angularC);
    if (J.enableLimit && J.limitState != LS_INACTIVE) {
        LeverArm(b1, J.localAnchor1, r1X, r1Y);
        LeverArm(b2, J.localAnchor2, r2X, r2Y);
        p1X = b1.sweep.c.x + r1X;
        p1Y = b1.sweep.c.y + r1Y;
        p2X = b2.sweep.c.x + r2X;
        p2Y = b2.sweep.c.y + r2Y;
        dX = p2X - p1X;
        dY = p2Y - p1Y;
        R = &b1.xf.R;
        double ax1X = R->col1.x * J.localXAxis1.x + R->col2.x * J.localXAxis1.y;
        double ax1Y = R->col1.y * J.localXAxis1.x + R->col2.y * J.localXAxis1.y;
        double translation = ax1X * dX + ax1Y * dY;
        double limitImpulse = 0;
        if (J.limitState == LS_EQUAL) {
            double limitC = b2Clamp(translation, -settings::maxLinearCorrection, settings::maxLinearCorrection);
            limitImpulse = -J.motorMass * limitC;
            positionError = b2Max(positionError, b2Abs(angularC));  // sic: angularC (AS3)
        } else if (J.limitState == LS_AT_LOWER) {
            double limitC = translation - J.lowerTranslation;
            positionError = b2Max(positionError, -limitC);
            limitC = b2Clamp(limitC + settings::linearSlop, -settings::maxLinearCorrection, 0);
            limitImpulse = -J.motorMass * limitC;
            double old = J.limitPositionImpulse;
            J.limitPositionImpulse = b2Max(J.limitPositionImpulse + limitImpulse, 0);
            limitImpulse = J.limitPositionImpulse - old;
        } else if (J.limitState == LS_AT_UPPER) {
            double limitC = translation - J.upperTranslation;
            positionError = b2Max(positionError, limitC);
            limitC = b2Clamp(limitC - settings::linearSlop, 0, settings::maxLinearCorrection);
            limitImpulse = -J.motorMass * limitC;
            double old = J.limitPositionImpulse;
            J.limitPositionImpulse = b2Min(J.limitPositionImpulse + limitImpulse, 0);
            limitImpulse = J.limitPositionImpulse - old;
        }
        ApplyJacobianPos(b1, b2, J.motorJacobian, invMass1, invMass2, invI1, invI2, limitImpulse);
        w.SynchronizeTransform(b1i);
        w.SynchronizeTransform(b2i);
    }
    return positionError <= settings::linearSlop && angularError <= settings::angularSlop;
}

// ================================================================ revolute

static void RevoluteK(Mat22& K, double invMass1, double invMass2, double invI1, double invI2, double r1X,
                      double r1Y, double r2X, double r2Y) {
    Mat22 K1, K2, K3;
    K1.col1.x = invMass1 + invMass2;
    K1.col2.x = 0;
    K1.col1.y = 0;
    K1.col2.y = invMass1 + invMass2;
    K2.col1.x = invI1 * r1Y * r1Y;
    K2.col2.x = -invI1 * r1X * r1Y;
    K2.col1.y = -invI1 * r1X * r1Y;
    K2.col2.y = invI1 * r1X * r1X;
    K3.col1.x = invI2 * r2Y * r2Y;
    K3.col2.x = -invI2 * r2X * r2Y;
    K3.col1.y = -invI2 * r2X * r2Y;
    K3.col2.y = invI2 * r2X * r2X;
    K = K1;  // SetM
    K.col1.x += K2.col1.x;  // AddM
    K.col1.y += K2.col1.y;
    K.col2.x += K2.col2.x;
    K.col2.y += K2.col2.y;
    K.col1.x += K3.col1.x;
    K.col1.y += K3.col1.y;
    K.col2.x += K3.col2.x;
    K.col2.y += K3.col2.y;
}

static void RevoluteInitVelocity(Joint& J, Body& b1, Body& b2, const TimeStep& step) {
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double invMass1 = b1.invMass, invMass2 = b2.invMass;
    double invI1 = b1.invI, invI2 = b2.invI;
    Mat22 K;
    RevoluteK(K, invMass1, invMass2, invI1, invI2, r1X, r1Y, r2X, r2Y);
    K.Invert(J.pivotMass);
    J.motorMass = 1 / (invI1 + invI2);
    if (J.enableMotor == false) J.motorForce = 0;
    if (J.enableLimit) {
        double jointAngle = b2.sweep.a - b1.sweep.a - J.referenceAngle;
        if (b2Abs(J.upperAngle - J.lowerAngle) < 2 * settings::angularSlop) {
            J.limitState = LS_EQUAL;
        } else if (jointAngle <= J.lowerAngle) {
            if (J.limitState != LS_AT_LOWER) J.limitForce = 0;
            J.limitState = LS_AT_LOWER;
        } else if (jointAngle >= J.upperAngle) {
            if (J.limitState != LS_AT_UPPER) J.limitForce = 0;
            J.limitState = LS_AT_UPPER;
        } else {
            J.limitState = LS_INACTIVE;
            J.limitForce = 0;
        }
    } else {
        J.limitForce = 0;
    }
    if (step.warmStarting) {
        b1.linearVelocity.x -= step.dt * invMass1 * J.pivotForce.x;
        b1.linearVelocity.y -= step.dt * invMass1 * J.pivotForce.y;
        b1.angularVelocity -=
            step.dt * invI1 * (r1X * J.pivotForce.y - r1Y * J.pivotForce.x + J.motorForce + J.limitForce);
        b2.linearVelocity.x += step.dt * invMass2 * J.pivotForce.x;
        b2.linearVelocity.y += step.dt * invMass2 * J.pivotForce.y;
        b2.angularVelocity +=
            step.dt * invI2 * (r2X * J.pivotForce.y - r2Y * J.pivotForce.x + J.motorForce + J.limitForce);
    } else {
        J.pivotForce.SetZero();
        J.motorForce = 0;
        J.limitForce = 0;
    }
    J.limitPositionImpulse = 0;
}

static void RevoluteSolveVelocity(Joint& J, Body& b1, Body& b2, const TimeStep& step) {
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double pivotCdotX = b2.linearVelocity.x + -b2.angularVelocity * r2Y - b1.linearVelocity.x -
                        -b1.angularVelocity * r1Y;
    double pivotCdotY = b2.linearVelocity.y + b2.angularVelocity * r2X - b1.linearVelocity.y -
                        b1.angularVelocity * r1X;
    double pivotForceX = -step.inv_dt * (J.pivotMass.col1.x * pivotCdotX + J.pivotMass.col2.x * pivotCdotY);
    double pivotForceY = -step.inv_dt * (J.pivotMass.col1.y * pivotCdotX + J.pivotMass.col2.y * pivotCdotY);
    J.pivotForce.x += pivotForceX;
    J.pivotForce.y += pivotForceY;
    double PX = step.dt * pivotForceX;
    double PY = step.dt * pivotForceY;
    b1.linearVelocity.x -= b1.invMass * PX;
    b1.linearVelocity.y -= b1.invMass * PY;
    b1.angularVelocity -= b1.invI * (r1X * PY - r1Y * PX);
    b2.linearVelocity.x += b2.invMass * PX;
    b2.linearVelocity.y += b2.invMass * PY;
    b2.angularVelocity += b2.invI * (r2X * PY - r2Y * PX);
    if (J.enableMotor && J.limitState != LS_EQUAL) {
        double motorCdot = b2.angularVelocity - b1.angularVelocity - J.motorSpeed;
        double motorForce = -step.inv_dt * J.motorMass * motorCdot;
        double old = J.motorForce;
        J.motorForce = b2Clamp(J.motorForce + motorForce, -J.maxMotorTorque, J.maxMotorTorque);
        motorForce = J.motorForce - old;
        b1.angularVelocity -= b1.invI * step.dt * motorForce;
        b2.angularVelocity += b2.invI * step.dt * motorForce;
    }
    if (J.enableLimit && J.limitState != LS_INACTIVE) {
        double limitCdot = b2.angularVelocity - b1.angularVelocity;
        double limitForce = -step.inv_dt * J.motorMass * limitCdot;
        if (J.limitState == LS_EQUAL) {
            J.limitForce += limitForce;
        } else if (J.limitState == LS_AT_LOWER) {
            double old = J.limitForce;
            J.limitForce = b2Max(J.limitForce + limitForce, 0);
            limitForce = J.limitForce - old;
        } else if (J.limitState == LS_AT_UPPER) {
            double old = J.limitForce;
            J.limitForce = b2Min(J.limitForce + limitForce, 0);
            limitForce = J.limitForce - old;
        }
        b1.angularVelocity -= b1.invI * step.dt * limitForce;
        b2.angularVelocity += b2.invI * step.dt * limitForce;
    }
}

static bool RevoluteSolvePosition(World& w, Joint& J, int32_t b1i, int32_t b2i) {
    Body& b1 = w.bodies[b1i];
    Body& b2 = w.bodies[b2i];
    double r1X, r1Y, r2X, r2Y;
    LeverArm(b1, J.localAnchor1, r1X, r1Y);
    LeverArm(b2, J.localAnchor2, r2X, r2Y);
    double p1X = b1.sweep.c.x + r1X;
    double p1Y = b1.sweep.c.y + r1Y;
    double p2X = b2.sweep.c.x + r2X;
    double p2Y = b2.sweep.c.y + r2Y;
    double ptpCX = p2X - p1X;
    double ptpCY = p2Y - p1Y;
    double positionError = as3_sqrt(ptpCX * ptpCX + ptpCY * ptpCY);
    Mat22 K;
    RevoluteK(K, b1.invMass, b2.invMass, b1.invI, b2.invI, r1X, r1Y, r2X, r2Y);
    Vec2 impulse = K.Solve(-ptpCX, -ptpCY);
    double impulseX = impulse.x, impulseY = impulse.y;
    b1.sweep.c.x -= b1.invMass * impulseX;
    b1.sweep.c.y -= b1.invMass * impulseY;
    b1.sweep.a -= b1.invI * (r1X * impulseY - r1Y * impulseX);
    b2.sweep.c.x += b2.invMass * impulseX;
    b2.sweep.c.y += b2.invMass * impulseY;
    b2.sweep.a += b2.invI * (r2X * impulseY - r2Y * impulseX);
    w.SynchronizeTransform(b1i);
    w.SynchronizeTransform(b2i);
    double angularError = 0;
    if (J.enableLimit && J.limitState != LS_INACTIVE) {
        double angle = b2.sweep.a - b1.sweep.a - J.referenceAngle;
        double limitImpulse = 0;
        if (J.limitState == LS_EQUAL) {
            double limitC = b2Clamp(angle, -settings::maxAngularCorrection, settings::maxAngularCorrection);
            limitImpulse = -J.motorMass * limitC;
            angularError = b2Abs(limitC);
        } else if (J.limitState == LS_AT_LOWER) {
            double limitC = angle - J.lowerAngle;
            angularError = b2Max(0, -limitC);
            limitC = b2Clamp(limitC + settings::angularSlop, -settings::maxAngularCorrection, 0);
            limitImpulse = -J.motorMass * limitC;
            double old = J.limitPositionImpulse;
            J.limitPositionImpulse = b2Max(J.limitPositionImpulse + limitImpulse, 0);
            limitImpulse = J.limitPositionImpulse - old;
        } else if (J.limitState == LS_AT_UPPER) {
            double limitC = angle - J.upperAngle;
            angularError = b2Max(0, limitC);
            limitC = b2Clamp(limitC - settings::angularSlop, 0, settings::maxAngularCorrection);
            limitImpulse = -J.motorMass * limitC;
            double old = J.limitPositionImpulse;
            J.limitPositionImpulse = b2Min(J.limitPositionImpulse + limitImpulse, 0);
            limitImpulse = J.limitPositionImpulse - old;
        }
        b1.sweep.a -= b1.invI * limitImpulse;
        b2.sweep.a += b2.invI * limitImpulse;
        w.SynchronizeTransform(b1i);
        w.SynchronizeTransform(b2i);
    }
    return positionError <= settings::linearSlop && angularError <= settings::angularSlop;
}

// ================================================================ dispatch

void World::JointInitVelocityConstraints(int32_t j, const TimeStep& step) {
    Joint& J = joints[j];
    Body& b1 = bodies[J.body1];
    Body& b2 = bodies[J.body2];
    switch (J.type) {
        case JT_DISTANCE: DistanceInitVelocity(J, b1, b2, step); break;
        case JT_PRISMATIC: PrismaticInitVelocity(J, b1, b2, step); break;
        case JT_REVOLUTE: RevoluteInitVelocity(J, b1, b2, step); break;
        default: fatal("joint type");
    }
}

void World::JointSolveVelocityConstraints(int32_t j, const TimeStep& step) {
    Joint& J = joints[j];
    Body& b1 = bodies[J.body1];
    Body& b2 = bodies[J.body2];
    switch (J.type) {
        case JT_DISTANCE: DistanceSolveVelocity(J, b1, b2); break;
        case JT_PRISMATIC: PrismaticSolveVelocity(J, b1, b2, step); break;
        case JT_REVOLUTE: RevoluteSolveVelocity(J, b1, b2, step); break;
        default: fatal("joint type");
    }
}

bool World::JointSolvePositionConstraints(int32_t j) {
    Joint& J = joints[j];
    switch (J.type) {
        case JT_DISTANCE: return DistanceSolvePosition(*this, J, J.body1, J.body2);
        case JT_PRISMATIC: return PrismaticSolvePosition(*this, J, J.body1, J.body2);
        case JT_REVOLUTE: return RevoluteSolvePosition(*this, J, J.body1, J.body2);
        default: fatal("joint type");
    }
}

}  // namespace rb
