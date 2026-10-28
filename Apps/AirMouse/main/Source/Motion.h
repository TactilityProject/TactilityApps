#pragma once

// Air-mouse motion pipeline: gyro bias calibration, gravity-based orientation
// mapping, smoothing, deadzone and sub-pixel accumulation.
//
// All IMU values entering this file are in a single, consistent body frame
// ("device frame"): +X = right, +Y = up (top of the screen), +Z = out of the
// screen toward the user. For devicetree IMUs the driver's mount-matrix has
// already produced that frame; for the bare external MPU6886 fallback the caller
// applies the same convention via MountMatrix.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace airmouse {

// Row-major 3x3 matrix using the Linux IIO "mount-matrix" convention:
//   x' = m[0]*x + m[1]*y + m[2]*z
//   y' = m[3]*x + m[4]*y + m[5]*z
//   z' = m[6]*x + m[7]*y + m[8]*z
// Maps the raw sensor frame onto the device frame described above.
struct MountMatrix {
    int32_t m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    void apply(float& x, float& y, float& z) const {
        const float rx = m[0] * x + m[1] * y + m[2] * z;
        const float ry = m[3] * x + m[4] * y + m[5] * z;
        const float rz = m[6] * x + m[7] * y + m[8] * z;
        x = rx;
        y = ry;
        z = rz;
    }
};

// One Euro filter: heavy smoothing at low speed, low latency at high speed.
// (Casiez, Roussel & Vogel, CHI 2012.) Ideal for pointer input - it removes the
// jitter/creep of a raw gyro without the lag of a fixed low-pass.
class OneEuroFilter {
public:
    void configure(float minCutoff, float beta, float dCutoff) {
        minCutoff_ = minCutoff;
        beta_ = beta;
        dCutoff_ = dCutoff;
    }

    void reset() { initialized_ = false; }

    float filter(float value, float dt) {
        if (dt <= 0.0f) dt = kFallbackDt;
        if (!initialized_) {
            initialized_ = true;
            xPrev_ = value;
            dxPrev_ = 0.0f;
            return value;
        }
        const float dx = (value - xPrev_) / dt;
        const float aD = alpha(dCutoff_, dt);
        const float dxHat = dxPrev_ + aD * (dx - dxPrev_);
        dxPrev_ = dxHat;
        const float cutoff = minCutoff_ + beta_ * std::fabs(dxHat);
        const float a = alpha(cutoff, dt);
        xPrev_ = xPrev_ + a * (value - xPrev_);
        return xPrev_;
    }

private:
    static constexpr float kFallbackDt = 1.0f / 33.0f;

    static float alpha(float cutoff, float dt) {
        const float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float minCutoff_ = 2.0f;
    float beta_ = 0.05f;
    float dCutoff_ = 1.0f;
    float xPrev_ = 0.0f;
    float dxPrev_ = 0.0f;
    bool initialized_ = false;
};

struct MotionConfig {
    float sensitivity = 1.0f;    // user multiplier (1.0 = stock feel)
    bool useOrientation = true;  // gravity-based axis mapping
    bool invertX = false;        // flip horizontal direction
    bool invertY = false;        // flip vertical direction
    // Freeze the pointer automatically while the device is being physically
    // repositioned. The manual clutch (both click buttons) is impossible on a
    // device with no touch screen, so this is the only way to "let go" there.
    bool autoClutch = false;
};

class MotionProcessor {
public:
    // Pointer response: pixels per (°/s) per second. Derived from the original
    // fixed-30ms constants (0.48 = 16.0 * 0.030, 0.285 = 9.5 * 0.030), now made
    // frame-rate independent.
    static constexpr float kGainHorizontal = 16.0f;
    static constexpr float kGainVertical = 9.5f;

    void configure(const MotionConfig& config) { config_ = config; }

    // Call when the pointer is (re)enabled: clears filters/accumulators and
    // restarts bias calibration. No pointer output is produced until a bias
    // estimate exists.
    void reset() {
        filterH_.reset();
        filterV_.reset();
        accumX_ = 0.0f;
        accumY_ = 0.0f;
        bias_[0] = bias_[1] = bias_[2] = 0.0f;
        biasValid_ = false;
        gravValid_ = false;
        autoClutchRun_ = 0;
        autoClutchQuiet_ = 0.0f;
        autoClutchActive_ = false;
        beginCalibration();
    }

    void beginCalibration() {
        calibCount_ = 0;
        calibTicks_ = 0;
        calibWindows_ = 0;
        calibrating_ = true;
    }

    bool isCalibrating() const { return calibrating_; }
    bool hasBias() const { return biasValid_; }
    // True while the pointer is being held back because the device is being moved
    // (only ever true with config_.autoClutch on).
    bool isAutoClutching() const { return autoClutchActive_; }
    float biasMagnitude() const {
        return std::sqrt(bias_[0] * bias_[0] + bias_[1] * bias_[1] + bias_[2] * bias_[2]);
    }
    // True when the last sample had enough gravity for orientation mapping.
    bool orientationActive() const { return orientValid_ && config_.useOrientation; }

    // Feeds one sample (accel in g, gyro in °/s, both device frame; dt in seconds).
    // Returns true and fills outDx/outDy when a pointer delta should be sent.
    bool update(float ax, float ay, float az,
                float gx, float gy, float gz,
                float dt, int& outDx, int& outDy) {
        if (dt <= 0.0f) dt = 1.0f / 33.0f;

        if (!updateBias(gx, gy, gz)) {
            return false; // still calibrating - don't steer with an unknown bias
        }
        gx -= bias_[0];
        gy -= bias_[1];
        gz -= bias_[2];

        updateAutoClutch(ax, ay, az, dt);

        float rateH;
        float rateV;
        if (config_.useOrientation && computeOrientation(ax, ay, az, gx, gy, gz, dt)) {
            // World-referenced: yaw about the gravity axis, pitch about the
            // horizontal axis the device "points" along. Consistent however the
            // device is held (flat, upright, portrait, landscape).
            rateH = yaw_;
            rateV = pitch_;
        } else {
            // Legacy flat-hold mapping (the original layout of this app).
            rateH = gz;
            rateV = -gy;
        }

        if (config_.invertX) rateH = -rateH;
        if (config_.invertY) rateV = -rateV;

        // Radial deadzone in °/s with a continuous, direction-preserving onset -
        // no per-axis "sticky cross".
        const float rateMag = std::sqrt(rateH * rateH + rateV * rateV);
        if (rateMag < kDeadzone) {
            rateH = 0.0f;
            rateV = 0.0f;
        } else {
            const float scale = (rateMag - kDeadzone) / rateMag;
            rateH *= scale;
            rateV *= scale;
        }

        rateH = filterH_.filter(rateH, dt);
        rateV = filterV_.filter(rateV, dt);

        float dxF = rateH * kGainHorizontal * config_.sensitivity * dt;
        float dyF = rateV * kGainVertical * config_.sensitivity * dt;

        // Sub-pixel accumulation: keep the fractional part so slow motion still
        // moves, instead of rounding every tick to zero.
        accumX_ += dxF;
        accumY_ += dyF;
        int dx = std::clamp(static_cast<int>(accumX_), -kMaxStep, kMaxStep);
        int dy = std::clamp(static_cast<int>(accumY_), -kMaxStep, kMaxStep);
        accumX_ -= static_cast<float>(dx);
        accumY_ -= static_cast<float>(dy);
        // The leftover is normally the sub-pixel fraction (<1). If the per-tick
        // clamp fired, discard the pent-up excess instead of "coasting" later.
        accumX_ = std::clamp(accumX_, -1.0f, 1.0f);
        accumY_ = std::clamp(accumY_, -1.0f, 1.0f);

        // Auto-clutch: the device is being moved, not steered - hold the pointer
        // still. The filters and the sub-pixel carry above keep running and drain
        // every tick, so nothing has piled up to lurch forward when it releases.
        if (autoClutchActive_) return false;

        if (dx == 0 && dy == 0) return false;
        outDx = dx;
        outDy = dy;
        return true;
    }

private:
    // Noise floor and calibration thresholds (°/s).
    static constexpr float kDeadzone = 1.5f;
    static constexpr int kMaxStep = 40;
    // Bias calibration window: the estimate is the per-axis median of that many samples.
    // The window is retried while the samples keep scattering, so that a moving hand is not
    // baked into the bias, and given up on after kCalibMaxTicks.
    static constexpr int kCalibSamples = 48;         // ~1.5s at 33Hz
    static constexpr float kCalibQuietRange = 4.0f;  // "still holding it" scatter around the median
    static constexpr int kCalibMaxTicks = 150;       // ~4.5s: use the best estimate we have
    static constexpr int kCalibMaxWindows = 4;
    static constexpr float kDriftMaxMag = 1.2f;
    static constexpr float kDriftRate = 0.01f;
    // Below this |accel| (g) we don't trust the gravity direction.
    static constexpr float kMinAccelMag = 0.3f;
    // Gravity tracking: time constant (s) of the low-pass that smooths the
    // gravity direction, and how far |accel| may deviate from 1 g before we stop
    // trusting it (linear acceleration from a hand movement).
    static constexpr float kGravTau = 0.12f;
    static constexpr float kGravTrustRange = 0.20f;

    // Auto-clutch. Steering rotates the device (mostly) in place, so |accel| stays
    // near 1 g; picking the device up or carrying it to a new position also
    // translates it, which adds linear acceleration on top of gravity and pushes
    // |accel| away from 1 g. That difference is the whole signal: enough sustained
    // "off 1 g" samples = being moved = freeze. A brisk wrist rotation can add a
    // little centripetal/tangential acceleration (~0.2 g worst case), which is why
    // the threshold sits above that and below a whole-arm carry (0.3-1 g).
    //
    // kAutoClutchRange is the main tuning knob: raise it if brisk steering ever
    // freezes the pointer, lower it if repositioning still flings it.
    static constexpr float kAutoClutchRange = 0.25f;        // |accel| deviation (g) counted as "being moved"
    static constexpr int   kAutoClutchEngageTicks = 2;      // consecutive such samples before freezing (~60ms)
    static constexpr float kAutoClutchReleaseTime = 0.25f;  // quiet time (s) before moving pointer again

    // Returns false while a bias estimate is still being formed.
    //
    // The estimate is a per-axis median rather than "the mean of the samples whose magnitude
    // stayed under X °/s". That threshold only works while the sensor's bias itself is small:
    // the samples it is meant to reject are the ones where the user's hand moved, but a sensor
    // biased by more than the threshold puts *every* sample over it, so nothing can be measured
    // at all and the estimate collapses to a zero bias (a breakout MPU6886 may sit at several
    // °/s; the datasheet allows ±20). A median does not care how large the bias is, and it
    // ignores the moving samples as long as they are a minority of the window.
    bool updateBias(float gx, float gy, float gz) {
        if (calibrating_) {
            if (calibCount_ < kCalibSamples) {
                calibBuf_[0][calibCount_] = gx;
                calibBuf_[1][calibCount_] = gy;
                calibBuf_[2][calibCount_] = gz;
                calibCount_++;
            }
            calibTicks_++;

            const bool windowFull = calibCount_ >= kCalibSamples;
            const bool outOfTime = calibTicks_ >= kCalibMaxTicks;
            if (windowFull || outOfTime) {
                const bool still = measureBias();
                // The hand was still moving: throw this window away and look at a fresh one,
                // unless we are out of patience (the pointer must not stay parked for ever).
                if (!still && !outOfTime && ++calibWindows_ < kCalibMaxWindows) {
                    calibCount_ = 0;
                    return false;
                }
                biasValid_ = true;
                calibrating_ = false;
                return true;
            }
            return false;
        }

        if (biasValid_) {
            // At rest, gently follow slow thermal drift of the bias. Note this follows a
            // drifting bias *continuously* (it only ever has to move by a fraction of the
            // range per second), which is what keeps a warming-up sensor from creeping.
            const float dx = gx - bias_[0];
            const float dy = gy - bias_[1];
            const float dz = gz - bias_[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < kDriftMaxMag) {
                bias_[0] += kDriftRate * dx;
                bias_[1] += kDriftRate * dy;
                bias_[2] += kDriftRate * dz;
            }
        }
        return true;
    }

    // Per-axis median of the collected window into bias_. Returns true when enough of the
    // window sits close to that median to call the device "held still" (the rest was the hand
    // moving, which the median has already ignored).
    bool measureBias() {
        if (calibCount_ == 0) {
            bias_[0] = bias_[1] = bias_[2] = 0.0f; // no samples at all (read failures)
            return true;
        }

        for (int axis = 0; axis < 3; axis++) {
            float sorted[kCalibSamples];
            for (int i = 0; i < calibCount_; i++) sorted[i] = calibBuf_[axis][i];
            for (int i = 1; i < calibCount_; i++) {
                const float value = sorted[i];
                int j = i - 1;
                while (j >= 0 && sorted[j] > value) {
                    sorted[j + 1] = sorted[j];
                    j--;
                }
                sorted[j + 1] = value;
            }
            bias_[axis] = sorted[calibCount_ / 2];
        }

        int quiet = 0;
        for (int i = 0; i < calibCount_; i++) {
            const float dx = calibBuf_[0][i] - bias_[0];
            const float dy = calibBuf_[1][i] - bias_[1];
            const float dz = calibBuf_[2][i] - bias_[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < kCalibQuietRange) quiet++;
        }
        // More than half the window has to sit near the median: a genuine "hold it still" sends
        // nearly every sample there, while a sustained rotation (carrying the device across the
        // room) leaves a large part of the window scattered, so a fresh window is worth trying.
        return quiet * 2 > calibCount_;
    }

    // Freezes the pointer while the device is being carried to a new position (so
    // the user can "let go" without a button chord) and releases it once the device
    // is quiet again. See the kAutoClutch* constants for the reasoning.
    void updateAutoClutch(float ax, float ay, float az, float dt) {
        if (!config_.autoClutch) {
            autoClutchRun_ = 0;
            autoClutchQuiet_ = 0.0f;
            autoClutchActive_ = false;
            return;
        }

        const float mag = std::sqrt(ax * ax + ay * ay + az * az);
        const float dev = std::fabs(mag - 1.0f);

        if (dev > kAutoClutchRange) {
            autoClutchQuiet_ = 0.0f;
            if (autoClutchRun_ < kAutoClutchEngageTicks) autoClutchRun_++;
            if (autoClutchRun_ >= kAutoClutchEngageTicks) autoClutchActive_ = true;
        } else {
            autoClutchRun_ = 0;
            if (autoClutchActive_) {
                autoClutchQuiet_ += dt;
                if (autoClutchQuiet_ >= kAutoClutchReleaseTime) {
                    autoClutchActive_ = false;
                    autoClutchQuiet_ = 0.0f;
                    // The gravity estimate was held (untrusted) for the whole move,
                    // so it still points the old way. Drop it and re-seed from the
                    // now-quiet device, so the axis mapping is correct the instant
                    // the pointer starts moving again instead of swinging over.
                    gravValid_ = false;
                }
            }
        }
    }

    // Low-pass estimates the gravity direction while the device is quiet. When
    // the accelerometer magnitude leaves 1 g (a hand movement adds linear
    // acceleration) the reading is contaminated, so the estimate is held instead
    // of followed - otherwise the axis mapping would swing mid-gesture and steer
    // the pointer the wrong way while the user physically reorients the device.
    void updateGravity(float ax, float ay, float az, float dt) {
        const float mag = std::sqrt(ax * ax + ay * ay + az * az);
        if (mag < kMinAccelMag) return; // free-fall / unusable
        const float nx = ax / mag;
        const float ny = ay / mag;
        const float nz = az / mag;

        if (!gravValid_) {
            gravX_ = nx;
            gravY_ = ny;
            gravZ_ = nz;
            gravValid_ = true;
            return;
        }

        const float dev = std::fabs(mag - 1.0f);
        float trust = 1.0f - dev / kGravTrustRange;
        if (trust < 0.0f) trust = 0.0f;
        else if (trust > 1.0f) trust = 1.0f;
        if (trust <= 0.0f) return;

        const float alpha = trust * (1.0f - std::exp(-dt / kGravTau));
        float bx = gravX_ + alpha * (nx - gravX_);
        float by = gravY_ + alpha * (ny - gravY_);
        float bz = gravZ_ + alpha * (nz - gravZ_);
        const float bm = std::sqrt(bx * bx + by * by + bz * bz);
        if (bm > 1e-3f) {
            gravX_ = bx / bm;
            gravY_ = by / bm;
            gravZ_ = bz / bm;
        }
    }

    // Derives yaw_/pitch_ (°/s) from the gyro and the gravity direction.
    // Returns false if gravity isn't usable (free-fall / bad read).
    bool computeOrientation(float ax, float ay, float az,
                            float gx, float gy, float gz, float dt) {
        updateGravity(ax, ay, az, dt);
        orientValid_ = gravValid_;
        if (!orientValid_) return false;

        const float ux = gravX_;
        const float uy = gravY_;
        const float uz = gravZ_; // world-up in the device frame (smoothed)

        // "Forward" reference for pitch = the direction the device *points*, i.e.
        // the screen normal (+Z) projected onto the horizontal plane. This is the
        // right choice held upright (screen vertical), and it's what makes the
        // mapping independent of how the device is rotated in the hand.
        const float ndot = uz;
        float nx = -ndot * ux;
        float ny = -ndot * uy;
        float nz = 1.0f - ndot * uz;
        const float nmag = std::sqrt(nx * nx + ny * ny + nz * nz);

        // When the device is lying flat the screen normal points along gravity and
        // gives no horizontal direction, so fall back to the screen top (+Y).
        const float tdot = uy;
        float tx = -tdot * ux;
        float ty = 1.0f - tdot * uy;
        float tz = -tdot * uz;
        const float tmag = std::sqrt(tx * tx + ty * ty + tz * tz);

        // Hysteresis: prefer the pointing direction whenever it has a usable
        // horizontal projection; only fall back to the top edge when nearly flat.
        if (usingNormalRef_) {
            if (nmag < 0.25f) usingNormalRef_ = false;
        } else if (nmag > 0.35f) {
            usingNormalRef_ = true;
        }

        float fx;
        float fy;
        float fz;
        if (usingNormalRef_ && nmag > 1e-3f) {
            // Negated so the pitch sign stays continuous across the flat/upright
            // transition (both agree at 45 degrees).
            fx = -nx / nmag;
            fy = -ny / nmag;
            fz = -nz / nmag;
        } else if (tmag > 1e-3f) {
            fx = tx / tmag;
            fy = ty / tmag;
            fz = tz / tmag;
        } else {
            fx = 0.0f;
            fy = 0.0f;
            fz = 0.0f;
        }

        // Horizontal "right" axis = up x forward.
        float rx = uy * fz - uz * fy;
        float ry = uz * fx - ux * fz;
        float rz = ux * fy - uy * fx;
        const float rmag = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rmag > 1e-3f) {
            rx /= rmag;
            ry /= rmag;
            rz /= rmag;
        } else {
            rx = 1.0f;
            ry = 0.0f;
            rz = 0.0f;
        }

        yaw_ = gx * ux + gy * uy + gz * uz;
        pitch_ = gx * rx + gy * ry + gz * rz;
        return true;
    }

    MotionConfig config_ {};

    OneEuroFilter filterH_ {};
    OneEuroFilter filterV_ {};

    float accumX_ = 0.0f;
    float accumY_ = 0.0f;

    float bias_[3] = { 0.0f, 0.0f, 0.0f };
    float calibBuf_[3][kCalibSamples] = {};
    int calibCount_ = 0;
    int calibTicks_ = 0;
    int calibWindows_ = 0;
    bool calibrating_ = false;
    bool biasValid_ = false;

    bool orientValid_ = false;
    bool usingNormalRef_ = true;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;

    // Smoothed gravity direction (device frame, unit vector).
    float gravX_ = 0.0f;
    float gravY_ = 0.0f;
    float gravZ_ = 0.0f;
    bool gravValid_ = false;

    // Auto-clutch state.
    int autoClutchRun_ = 0;         // consecutive "being moved" samples
    float autoClutchQuiet_ = 0.0f;  // seconds of quiet accumulated while engaged
    bool autoClutchActive_ = false;
};

} // namespace airmouse
