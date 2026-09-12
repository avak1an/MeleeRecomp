/**
 * @file mtx.c
 * Plain C versions of the paired-single (PS*) matrix and vector routines the
 * game calls directly. The SDK's own C_ variants live next to PowerPC
 * assembly in extern/dolphin/src/dolphin/mtx and are not compiled on PC;
 * the pure-C mtx44.c (projection matrices) is compiled as-is.
 */
#include <dolphin/mtx.h>

#include <math.h>
#include <string.h>

void PSMTXIdentity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    if (src != dst) {
        memcpy(dst, src, sizeof(Mtx));
    }
}

void PSMTXConcat(Mtx a, Mtx b, Mtx ab)
{
    Mtx tmp;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
        tmp[i][3] += a[i][3];
    }
    memcpy(ab, tmp, sizeof(Mtx));
}

void PSMTXTranspose(Mtx src, Mtx xPose)
{
    Mtx tmp;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            tmp[i][j] = src[j][i];
        }
        tmp[i][3] = 0.0f;
    }
    memcpy(xPose, tmp, sizeof(Mtx));
}

u32 PSMTXInverse(Mtx src, Mtx inv)
{
    Mtx tmp;
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] +
              src[0][2] * src[1][0] * src[2][1] - src[2][0] * src[1][1] * src[0][2] -
              src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    f32 rdet;
    if (det == 0.0f) {
        return 0;
    }
    rdet = 1.0f / det;
    tmp[0][0] = rdet * (src[1][1] * src[2][2] - src[2][1] * src[1][2]);
    tmp[0][1] = -rdet * (src[0][1] * src[2][2] - src[2][1] * src[0][2]);
    tmp[0][2] = rdet * (src[0][1] * src[1][2] - src[1][1] * src[0][2]);
    tmp[1][0] = -rdet * (src[1][0] * src[2][2] - src[2][0] * src[1][2]);
    tmp[1][1] = rdet * (src[0][0] * src[2][2] - src[2][0] * src[0][2]);
    tmp[1][2] = -rdet * (src[0][0] * src[1][2] - src[1][0] * src[0][2]);
    tmp[2][0] = rdet * (src[1][0] * src[2][1] - src[2][0] * src[1][1]);
    tmp[2][1] = -rdet * (src[0][0] * src[2][1] - src[2][0] * src[0][1]);
    tmp[2][2] = rdet * (src[0][0] * src[1][1] - src[1][0] * src[0][1]);
    tmp[0][3] = -tmp[0][0] * src[0][3] - tmp[0][1] * src[1][3] - tmp[0][2] * src[2][3];
    tmp[1][3] = -tmp[1][0] * src[0][3] - tmp[1][1] * src[1][3] - tmp[1][2] * src[2][3];
    tmp[2][3] = -tmp[2][0] * src[0][3] - tmp[2][1] * src[1][3] - tmp[2][2] * src[2][3];
    memcpy(inv, tmp, sizeof(Mtx));
    return 1;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    PSMTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    Vec v;
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    PSVECNormalize(axis, &v);
    m[0][0] = t * v.x * v.x + c;
    m[0][1] = t * v.x * v.y - s * v.z;
    m[0][2] = t * v.x * v.z + s * v.y;
    m[0][3] = 0.0f;
    m[1][0] = t * v.x * v.y + s * v.z;
    m[1][1] = t * v.y * v.y + c;
    m[1][2] = t * v.y * v.z - s * v.x;
    m[1][3] = 0.0f;
    m[2][0] = t * v.x * v.z - s * v.y;
    m[2][1] = t * v.y * v.z + s * v.x;
    m[2][2] = t * v.z * v.z + c;
    m[2][3] = 0.0f;
}

void PSMTXQuat(Mtx m, QuaternionPtr q)
{
    f32 s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    f32 xs = q->x * s, ys = q->y * s, zs = q->z * s;
    f32 wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    f32 xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    f32 yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;
    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}

void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    Vec t;
    t.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z + m[0][3];
    t.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z + m[1][3];
    t.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z + m[2][3];
    *dst = t;
}

void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    Vec t;
    t.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z;
    t.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z;
    t.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z;
    *dst = t;
}

void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target)
{
    Vec vLook, vRight, vUp;
    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    PSVECNormalize(&vLook, &vLook);
    PSVECCrossProduct(camUp, &vLook, &vRight);
    PSVECNormalize(&vRight, &vRight);
    PSVECCrossProduct(&vLook, &vRight, &vUp);
    m[0][0] = vRight.x;
    m[0][1] = vRight.y;
    m[0][2] = vRight.z;
    m[0][3] = -(camPos->x * vRight.x + camPos->y * vRight.y + camPos->z * vRight.z);
    m[1][0] = vUp.x;
    m[1][1] = vUp.y;
    m[1][2] = vUp.z;
    m[1][3] = -(camPos->x * vUp.x + camPos->y * vUp.y + camPos->z * vUp.z);
    m[2][0] = vLook.x;
    m[2][1] = vLook.y;
    m[2][2] = vLook.z;
    m[2][3] = -(camPos->x * vLook.x + camPos->y * vLook.y + camPos->z * vLook.z);
}

void PSVECAdd(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x + b->x;
    c->y = a->y + b->y;
    c->z = a->z + b->z;
}

void PSVECSubtract(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x - b->x;
    c->y = a->y - b->y;
    c->z = a->z - b->z;
}

void PSVECScale(Vec* src, Vec* dst, f32 scale)
{
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

f32 PSVECDotProduct(Vec* a, Vec* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

f32 PSVECMag(Vec* v)
{
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

void PSVECNormalize(Vec* src, Vec* dst)
{
    f32 mag = src->x * src->x + src->y * src->y + src->z * src->z;
    f32 r = mag > 0.0f ? 1.0f / sqrtf(mag) : 0.0f;
    dst->x = src->x * r;
    dst->y = src->y * r;
    dst->z = src->z * r;
}

void PSVECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
    Vec t;
    t.x = a->y * b->z - a->z * b->y;
    t.y = a->z * b->x - a->x * b->z;
    t.z = a->x * b->y - a->y * b->x;
    *axb = t;
}

/* --- Rotation and light-projection matrices ---------------------------------
 * The SDK's C versions (mtx.c); they were stubs, which left the fighter
 * shadow projections, the refraction texture matrix (cloaking device, heat
 * haze) and rotated billboards (rotated effect sprites) with zero
 * matrices. */

void MTXRotRad(Mtx m, char axis, f32 rad)
{
    f32 sn = sinf(rad), cs = cosf(rad);
    axis |= 0x20;
    switch (axis) {
    case 'x':
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cs;   m[1][2] = -sn;  m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sn;   m[2][2] = cs;   m[2][3] = 0.0f;
        break;
    case 'y':
        m[0][0] = cs;   m[0][1] = 0.0f; m[0][2] = sn;   m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = -sn;  m[2][1] = 0.0f; m[2][2] = cs;   m[2][3] = 0.0f;
        break;
    case 'z':
        m[0][0] = cs;   m[0][1] = -sn;  m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sn;   m[1][1] = cs;   m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        break;
    }
}

void MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 tmp = 1.0f / (r - l);
    m[0][0] = scaleS * (2.0f * n * tmp);
    m[0][1] = 0.0f;
    m[0][2] = scaleS * (tmp * (r + l)) - transS;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = scaleT * (2.0f * n * tmp);
    m[1][2] = scaleT * (tmp * (t + b)) - transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 angle = 0.5f * fovY * 0.017453293f;
    f32 cot = 1.0f / tanf(angle);
    m[0][0] = scaleS * (cot / aspect);
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = transS + scaleS * (tmp * -(r + l));
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = transT + scaleT * (tmp * -(t + b));
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}
