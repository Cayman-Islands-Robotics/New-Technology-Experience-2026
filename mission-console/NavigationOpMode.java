package org.firstinspires.ftc.teamcode;

import com.qualcomm.hardware.rev.RevHubOrientationOnRobot;
import com.qualcomm.robotcore.eventloop.opmode.LinearOpMode;
import com.qualcomm.robotcore.eventloop.opmode.TeleOp;
import com.qualcomm.robotcore.hardware.DcMotor;
import com.qualcomm.robotcore.hardware.DcMotorSimple;
import com.qualcomm.robotcore.hardware.IMU;

import org.firstinspires.ftc.robotcore.external.navigation.AngleUnit;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

import org.firstinspires.ftc.robotcore.external.navigation.AxesOrder;
import org.firstinspires.ftc.robotcore.external.navigation.AxesReference;
import org.firstinspires.ftc.robotcore.external.navigation.Orientation;

/**
 * NavigationOpMode
 *
 * Phase 1: proves the GPS pipeline end to end. Runs a TCP server on
 * port 6000 (the same port hub_link.py's HubLink connects out to as a
 * client), reads newline-delimited JSON "target" messages, and shows
 * the latest fix on Driver Station telemetry.
 *
 * Matches the wire protocol documented in hub_link.py:
 *   Pi -> Hub: {"type":"target","cur_lat":..,"cur_lon":..,
 *               "target_lat":..,"target_lon":..,"fix_id":<int>,
 *               "wp_id":<int>,"hdop":<float|null>,"num_sats":<int|null>,
 *               "seq":<int>,"ts":<unix>}
 *   Hub -> Pi: {"type":"status","state":..,"dr_lat":..,"dr_lon":..,
 *               "heading_deg":..,"heading_calibrated":bool,"wp_id":<int>,
 *               "distance_m":..,"estop":bool,"ts":<unix>}
 *
 * Phase 2 addition: "Align to North". Once a fresh GPS fix is present,
 * pressing gamepad1.a drives the robot forward a short distance to
 * establish its current compass bearing, turns in place to face true
 * north using the Control Hub IMU, then zeros the IMU yaw so "IMU
 * heading 0" means "facing true north" from then on.
 *
 * The "how far have we driven" check during that forward leg is done
 * with drivetrain encoders, not live GPS -- GPS position noise made the
 * old haversine-per-tick distance estimate unreliable while in motion.
 * GPS is still used for exactly two samples: the fix at the moment the
 * drive starts (alignStartLat/Lon) and the fix at the moment the
 * encoder-measured displacement threshold is reached. The bearing
 * between those two clean endpoint fixes is what we turn to.
 *
 * Phase 3 addition: "Waypoint test". Once aligned (gamepad1.a -> DONE),
 * pressing gamepad1.b captures the current fix as "home" and drives to
 * a hardcoded test waypoint (WAYPOINT_TEST_LAT/LON below -- EDIT THESE
 * before running), then waits for a second gamepad1.b press to drive
 * back to the captured home point. Unlike Align-to-North, this is a
 * *closed-loop* drive: bearing-to-target and distance-to-target are
 * recomputed from the live GPS fix every control tick (~10Hz), not
 * calculated once at the start. That means a residual heading error
 * from the align step, or IMU drift over the course of the leg, gets
 * continuously nudged out rather than locked in for the whole drive.
 * The IMU is still used between GPS updates for smooth short-term turn
 * control -- only the *target* heading is GPS-driven, not the
 * moment-to-moment steering signal.
 */
@TeleOp(name = "NavigationOpMode", group = "Nav")
public class NavigationOpMode extends LinearOpMode {

    private static final int PORT = 6000;
    // A fresh fix should arrive every 200ms (gps_uplink.py runs at 5Hz).
    // Call it stale after 3s -- long enough to survive a hiccup, short
    // enough that a dead Pi/adb link shows up quickly.
    private static final long STALE_MS = 3000;

    // ---- Align-to-North tuning constants ----
    private static final double ALIGN_DRIVE_POWER = 0.35;
    private static final double ALIGN_MIN_DISPLACEMENT_M = 2.0;
    private static final long ALIGN_DRIVE_TIMEOUT_MS = 8000;
    private static final double ALIGN_TURN_TOLERANCE_DEG = 2.0;
    private static final long ALIGN_TURN_TIMEOUT_MS = 5000;
    // Measured with TurnPowerCalibration: this drivetrain doesn't
    // actually start rotating until roughly 0.75-0.8 power, so the old
    // 0.12/0.4 floor and ceiling were both well under the real deadband
    // and the turn would just stall. Floor is set just under the
    // measured threshold; ceiling gives a little headroom above it.
    private static final double ALIGN_TURN_MAX_POWER = 0.95;
    private static final double ALIGN_TURN_MIN_POWER = 0.99;
    // Error (deg) at which the turn's proportional control saturates to
    // ALIGN_TURN_MAX_POWER. Below this the turn slows proportionally.
    private static final double ALIGN_TURN_FULL_POWER_ERROR_DEG = 90.0;

    // ---- Waypoint-drive tuning constants ----
    //
    // EDIT THESE before running the waypoint test. Placeholder (0,0) is
    // deliberately invalid so a forgotten edit fails loudly (huge
    // distance / nonsense bearing) instead of silently driving toward
    // the Gulf of Guinea.
    private static final double WAYPOINT_TEST_LAT = 19.31954573333;
    private static final double WAYPOINT_TEST_LON = -81.3770070833;

    // Within this radius of the target, consider it "arrived". GPS
    // noise on this modem (autonomous, no RTK) is commonly 2-5m, so
    // going much tighter than this risks the robot orbiting a target it
    // can never quite convince itself it has reached. Loosen this if
    // you see that happen; tighten it once you've characterized actual
    // scatter for your antenna/sky view (see gnss_reader.py's stationary
    // logging note).
    private static final double WP_ARRIVE_RADIUS_M = 2.0;

    // If heading error exceeds this, stop driving forward and turn in
    // place first rather than arcing toward the target -- avoids
    // wide swings off a bad initial heading.
    private static final double WP_TURN_IN_PLACE_THRESHOLD_DEG = 30.0;

    // Forward power while driving a leg, and while turning in place at
    // the start of a leg (reuses the align turn power band since it's
    // already been measured against this drivetrain's real deadband).
    private static final double WP_DRIVE_FORWARD_POWER = 0.35;
    private static final double WP_TURN_IN_PLACE_MAX_POWER = ALIGN_TURN_MAX_POWER;
    private static final double WP_TURN_IN_PLACE_MIN_POWER = ALIGN_TURN_MIN_POWER;
    private static final double WP_TURN_IN_PLACE_FULL_POWER_ERROR_DEG = ALIGN_TURN_FULL_POWER_ERROR_DEG;

    // While driving forward (heading error already under the
    // turn-in-place threshold), steering correction is a much gentler
    // proportional nudge layered on top of forward power -- not the
    // aggressive stall-busting power band used to spin in place. This
    // keeps the drivetrain from swinging past straight-ahead pointing.
    private static final double WP_STEER_MAX_TURN_POWER = 0.30;
    private static final double WP_STEER_FULL_POWER_ERROR_DEG = WP_TURN_IN_PLACE_THRESHOLD_DEG;

    // Start slowing forward power down within this many meters of the
    // target, to reduce GPS-noise-driven overshoot right at arrival.
    private static final double WP_SLOWDOWN_RADIUS_M = 3.0;
    private static final double WP_MIN_FORWARD_POWER = 0.18;

    // Safety bounds. A leg that can't reach WP_ARRIVE_RADIUS_M inside
    // this time has some other problem (bad heading, stuck robot, wrong
    // coordinates) and should stop rather than run indefinitely.
    private static final long WP_LEG_TIMEOUT_MS = 60000;
    private static final long WP_STALE_ABORT_MS = 5000;

    private static final double EARTH_RADIUS_M = 6371000.0;

    // ---- Drivetrain encoder -> distance conversion ----
    // REV Ultra Planetary cartridges sit on a REV HD Hex motor, whose
    // encoder resolution is 28 ticks per revolution of the bare motor
    // shaft (before the planetary reduction). NOTE: REV's "1:25"
    // cartridge stack is nominally 25:1 but the real ratio is closer to
    // 24.5:1 -- if you need tighter precision, spin the wheel exactly
    // one full revolution by hand and read the actual tick delta to
    // replace ULTRA_PLANETARY_GEAR_RATIO below.
    private static final double ENCODER_TICKS_PER_MOTOR_REV = 28.0;
    private static final double ULTRA_PLANETARY_GEAR_RATIO = 25.0;
    private static final double WHEEL_DIAMETER_IN = 10.0;
    private static final double METERS_PER_INCH = 0.0254;
    private static final double WHEEL_DIAMETER_M = WHEEL_DIAMETER_IN * METERS_PER_INCH;
    private static final double WHEEL_CIRCUMFERENCE_M = Math.PI * WHEEL_DIAMETER_M;
    private static final double TICKS_PER_WHEEL_REV = ENCODER_TICKS_PER_MOTOR_REV * ULTRA_PLANETARY_GEAR_RATIO;
    private static final double TICKS_PER_METER = TICKS_PER_WHEEL_REV / WHEEL_CIRCUMFERENCE_M;

    private ServerSocket serverSocket;
    private volatile Socket clientSocket;
    private Thread serverThread;
    private final AtomicBoolean running = new AtomicBoolean(true);

    // Written by the socket thread, read by the telemetry loop.
    private final AtomicReference<JSONObject> latestTarget = new AtomicReference<>(null);
    private final AtomicLong lastMessageAt = new AtomicLong(0);
    private final AtomicLong messageCount = new AtomicLong(0);
    private volatile String connectionState = "starting server...";

    // ---- Drivetrain / IMU ----
    private DcMotor fl, fr, rl, rr;
    private IMU imu;

    // ---- Align-to-North state ----
    private enum AlignState { IDLE, DRIVING, TURNING, DONE, ABORTED }

    private AlignState alignState = AlignState.IDLE;
    private boolean lastAPressed = false;
    private double alignStartLat, alignStartLon;
    private int alignStartFlTicks, alignStartFrTicks, alignStartRlTicks, alignStartRrTicks;
    private long alignPhaseStartTime;
    private double alignBearingDeg;
    private double alignTurnTargetYaw;
    private String alignErrorMessage = "";

    // ---- Waypoint-drive state ----
    private enum WaypointState { IDLE, TO_WAYPOINT, ARRIVED_WP, RETURN_HOME, ARRIVED_HOME, ABORTED }

    private WaypointState wpState = WaypointState.IDLE;
    private boolean lastBPressed = false;
    private double wpHomeLat, wpHomeLon;
    private long wpLegStartTime;
    private String wpErrorMessage = "";
    // Telemetry-only snapshot of the last control tick's numbers, so the
    // driver can see what the controller is actually doing.
    private double wpLastBearingDeg, wpLastDistanceM, wpLastHeadingErrorDeg;

    @Override
    public void runOpMode() {
        startServer();
        initDrivetrainAndImu();

        telemetry.addLine("NavigationOpMode loaded.");
        telemetry.addLine("Waiting for the Pi to connect on port " + PORT + "...");
        telemetry.update();

        waitForStart();

        while (opModeIsActive()) {
            JSONObject target = latestTarget.get();
            long lastAt = lastMessageAt.get();
            long age = lastAt == 0 ? -1 : System.currentTimeMillis() - lastAt;
            boolean fresh = lastAt != 0 && age < STALE_MS;

            telemetry.addData("Link", connectionState);
            telemetry.addData("Messages received", messageCount.get());

            if (target == null) {
                if ("Pi connected".equals(connectionState)) {
                    // The Pi is connected but gps_uplink.py hasn't sent a
                    // "target" message yet -- it only sends one when
                    // gnss_reader.py has a fresh fix, and stays silent
                    // (calls hub.clear_target()) otherwise. So silence
                    // here, on a live connection, means the modem is
                    // still acquiring satellites.
                    telemetry.addLine("Searching for satellites...");
                    telemetry.addLine("Pi is connected, waiting on first GPS fix.");
                    telemetry.addLine("Cold start outdoors can take a few minutes.");
                } else {
                    telemetry.addLine("No GPS data received yet.");
                    telemetry.addLine("Check: is start_gps_uplink.sh running on the Pi?");
                    telemetry.addLine("Check: adb forward tcp:6000 tcp:6000 established?");
                }
            } else if (!fresh) {
                // Same "no fresh fix" condition as above, but here it
                // happened after we'd already gotten at least one real
                // fix -- show the last known position alongside it.
                telemetry.addData("Status", "Searching for satellites... (lost fix, %d ms ago)", age);
                telemetry.addData("Last known lat/lon", "%.6f, %.6f",
                        target.optDouble("cur_lat", 0), target.optDouble("cur_lon", 0));
                telemetry.addLine("If this persists: check antenna sky view or the gps_uplink service.");
            } else {
                telemetry.addData("Lat", "%.15f", target.optDouble("cur_lat", 0));
                telemetry.addData("Lon", "%.15f", target.optDouble("cur_lon", 0));
                if (!target.isNull("hdop")) {
                    telemetry.addData("HDOP", target.optDouble("hdop", 0));
                } else {
                    telemetry.addData("HDOP", "n/a");
                }
                if (!target.isNull("num_sats")) {
                    telemetry.addData("Sats", target.optInt("num_sats", -1));
                } else {
                    telemetry.addData("Sats", "n/a");
                }
                telemetry.addData("Fix ID", target.optInt("fix_id", -1));
                telemetry.addData("Age", "%d ms", age);
            }

            updateAlignment(target, fresh);
            updateWaypointTest(target, fresh);

            telemetry.update();
            sleep(100);
        }

        // Make sure we never leave the field with motors spinning if the
        // OpMode is stopped mid-maneuver.
        setDrivePower(0);
        shutdownServer();
    }

    // ------------------------------------------------------------------
    // Waypoint drive test (Phase 3)
    // ------------------------------------------------------------------

    /**
     * Runs one tick of the "drive to waypoint, then back home" test and
     * adds telemetry describing its current phase. Only reachable once
     * Align-to-North has completed (alignState == DONE) -- the whole
     * point of this test is to check waypoint driving against a known
     * heading reference, so starting it unaligned would just confound
     * the result.
     */
    private void updateWaypointTest(JSONObject target, boolean fresh) {
        boolean bPressed = gamepad1.b;
        boolean bEdge = bPressed && !lastBPressed;
        lastBPressed = bPressed;

        boolean alignedReady = alignState == AlignState.DONE;

        switch (wpState) {
            case IDLE:
                if (alignedReady) {
                    telemetry.addLine("Press B to drive to test waypoint, then home.");
                    if (WAYPOINT_TEST_LAT == 0.0 && WAYPOINT_TEST_LON == 0.0) {
                        telemetry.addLine("WARNING: WAYPOINT_TEST_LAT/LON still at placeholder (0,0) -- edit before running!");
                    }
                    if (bEdge && fresh) {
                        wpHomeLat = target.optDouble("cur_lat", 0);
                        wpHomeLon = target.optDouble("cur_lon", 0);
                        wpLegStartTime = System.currentTimeMillis();
                        wpErrorMessage = "";
                        wpState = WaypointState.TO_WAYPOINT;
                    }
                } else {
                    telemetry.addLine("Align to North first (press A) before the waypoint test.");
                }
                break;

            case TO_WAYPOINT: {
                telemetry.addLine("Waypoint test: driving to target...");
                WpStepResult result = driveTowardTarget(target, fresh, WAYPOINT_TEST_LAT, WAYPOINT_TEST_LON);
                reportWpStep();
                if (result == WpStepResult.ARRIVED) {
                    setDrivePower(0);
                    wpState = WaypointState.ARRIVED_WP;
                } else if (result == WpStepResult.ABORT) {
                    wpState = WaypointState.ABORTED;
                }
                break;
            }

            case ARRIVED_WP:
                telemetry.addLine("Arrived at waypoint!");
                telemetry.addLine("Press B to drive back home.");
                if (bEdge) {
                    wpLegStartTime = System.currentTimeMillis();
                    wpState = WaypointState.RETURN_HOME;
                }
                break;

            case RETURN_HOME: {
                telemetry.addLine("Waypoint test: returning home...");
                WpStepResult result = driveTowardTarget(target, fresh, wpHomeLat, wpHomeLon);
                reportWpStep();
                if (result == WpStepResult.ARRIVED) {
                    setDrivePower(0);
                    wpState = WaypointState.ARRIVED_HOME;
                } else if (result == WpStepResult.ABORT) {
                    wpState = WaypointState.ABORTED;
                }
                break;
            }

            case ARRIVED_HOME:
                telemetry.addLine("Back home! Round trip complete.");
                telemetry.addLine("Press B to run the test again.");
                if (bEdge && fresh) {
                    wpHomeLat = target.optDouble("cur_lat", 0);
                    wpHomeLon = target.optDouble("cur_lon", 0);
                    wpLegStartTime = System.currentTimeMillis();
                    wpState = WaypointState.TO_WAYPOINT;
                }
                break;

            case ABORTED:
                telemetry.addData("Waypoint test aborted", wpErrorMessage);
                telemetry.addLine("Press B to retry from home capture.");
                if (bEdge && fresh) {
                    wpHomeLat = target.optDouble("cur_lat", 0);
                    wpHomeLon = target.optDouble("cur_lon", 0);
                    wpLegStartTime = System.currentTimeMillis();
                    wpErrorMessage = "";
                    wpState = WaypointState.TO_WAYPOINT;
                }
                break;
        }
    }

    private enum WpStepResult { CONTINUE, ARRIVED, ABORT }

    /**
     * One control tick of closed-loop drive toward (targetLat, targetLon).
     * Bearing-to-target and distance-to-target are recomputed from the
     * live GPS fix on every call -- this is the "live correction" part:
     * there is no single bearing calculated once at the start of the
     * leg, so a noisy align, IMU drift, or drivetrain skew all get
     * continuously corrected against rather than compounding unseen
     * over the whole leg.
     *
     * The IMU is still what's actually being steered against tick to
     * tick (it updates far faster than the ~1Hz GPS fix rate), but the
     * *target* heading it's steering toward is refreshed from GPS every
     * time a new fix comes in.
     */
    private WpStepResult driveTowardTarget(JSONObject target, boolean fresh, double targetLat, double targetLon) {
        if (!fresh) {
            long staleFor = lastMessageAt.get() == 0
                    ? Long.MAX_VALUE
                    : System.currentTimeMillis() - lastMessageAt.get();
            if (staleFor > WP_STALE_ABORT_MS) {
                setDrivePower(0);
                wpErrorMessage = "GPS fix lost for over " + WP_STALE_ABORT_MS + "ms during drive.";
                return WpStepResult.ABORT;
            }
            // Brief hiccup: hold position rather than driving blind on a
            // stale fix, but don't abort yet -- same tolerance as the
            // main telemetry loop's STALE_MS reasoning.
            setDrivePower(0);
            return WpStepResult.CONTINUE;
        }

        long elapsed = System.currentTimeMillis() - wpLegStartTime;
        if (elapsed > WP_LEG_TIMEOUT_MS) {
            setDrivePower(0);
            wpErrorMessage = "Leg timed out after " + WP_LEG_TIMEOUT_MS + "ms without reaching arrival radius.";
            return WpStepResult.ABORT;
        }

        double curLat = target.optDouble("cur_lat", 0);
        double curLon = target.optDouble("cur_lon", 0);

        double distanceM = computeDistanceMeters(curLat, curLon, targetLat, targetLon);
        double bearingToTargetDeg = computeBearingDeg(curLat, curLon, targetLat, targetLon);
        double currentYaw = imu.getRobotYawPitchRollAngles().getYaw(AngleUnit.DEGREES);
        double headingErrorDeg = wrapAngle180(bearingToTargetDeg - currentYaw);

        wpLastBearingDeg = bearingToTargetDeg;
        wpLastDistanceM = distanceM;
        wpLastHeadingErrorDeg = headingErrorDeg;

        if (distanceM <= WP_ARRIVE_RADIUS_M) {
            setDrivePower(0);
            return WpStepResult.ARRIVED;
        }

        if (Math.abs(headingErrorDeg) > WP_TURN_IN_PLACE_THRESHOLD_DEG) {
            // Pointed too far off to drive forward usefully -- turn in
            // place first, same proportional-with-floor scheme as
            // Align-to-North's turn phase (this drivetrain has a real
            // ~0.75-0.8 power deadband before it starts rotating at all;
            // see ALIGN_TURN_MIN_POWER's comment).
            double power = headingErrorDeg / WP_TURN_IN_PLACE_FULL_POWER_ERROR_DEG;
            power = Math.max(-WP_TURN_IN_PLACE_MAX_POWER, Math.min(WP_TURN_IN_PLACE_MAX_POWER, power));
            if (Math.abs(power) < WP_TURN_IN_PLACE_MIN_POWER) {
                power = Math.copySign(WP_TURN_IN_PLACE_MIN_POWER, power);
            }
            setTurnPower(power);
        } else {
            // Roughly pointed the right way -- drive forward with a
            // gentle proportional steering correction layered on top,
            // slowing down as we approach to limit GPS-noise overshoot.
            double forwardPower = WP_DRIVE_FORWARD_POWER;
            if (distanceM < WP_SLOWDOWN_RADIUS_M) {
                double t = distanceM / WP_SLOWDOWN_RADIUS_M; // 0..1
                forwardPower = WP_MIN_FORWARD_POWER + (WP_DRIVE_FORWARD_POWER - WP_MIN_FORWARD_POWER) * t;
            }

            double turnPower = (headingErrorDeg / WP_STEER_FULL_POWER_ERROR_DEG) * WP_STEER_MAX_TURN_POWER;
            turnPower = Math.max(-WP_STEER_MAX_TURN_POWER, Math.min(WP_STEER_MAX_TURN_POWER, turnPower));

            driveMix(forwardPower, turnPower);
        }

        return WpStepResult.CONTINUE;
    }

    private void reportWpStep() {
        telemetry.addData("Bearing to target", "%.1f deg", wpLastBearingDeg);
        telemetry.addData("Distance to target", "%.2f m", wpLastDistanceM);
        telemetry.addData("Heading error", "%.1f deg", wpLastHeadingErrorDeg);
    }

    /**
     * Combined forward-drive + turn-correction motor mixing. Generalizes
     * setDrivePower(forward) and setTurnPower(turn) into one call so they
     * can be applied simultaneously: driveMix(forward, 0) is identical
     * to setDrivePower(forward), and driveMix(0, turn) is identical to
     * setTurnPower(turn). Keeps the same front-motors-vs-rear-motors
     * turn convention documented on setTurnPower (a hardware quirk on
     * this drivetrain, not an anatomical left/right split).
     */
    private void driveMix(double forward, double turn) {
        double flP = clampPower(forward - turn);
        double frP = clampPower(forward - turn);
        double rlP = clampPower(forward + turn);
        double rrP = clampPower(forward + turn);
        fl.setPower(flP);
        fr.setPower(frP);
        rl.setPower(rlP);
        rr.setPower(rrP);
    }

    private static double clampPower(double p) {
        return Math.max(-1.0, Math.min(1.0, p));
    }

    /** Great-circle distance in meters between two lat/lon points (haversine). */
    private static double computeDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
        double phi1 = Math.toRadians(lat1);
        double phi2 = Math.toRadians(lat2);
        double dPhi = Math.toRadians(lat2 - lat1);
        double dLambda = Math.toRadians(lon2 - lon1);

        double a = Math.sin(dPhi / 2) * Math.sin(dPhi / 2)
                + Math.cos(phi1) * Math.cos(phi2) * Math.sin(dLambda / 2) * Math.sin(dLambda / 2);
        double c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
        return EARTH_RADIUS_M * c;
    }

    // ------------------------------------------------------------------
    // Align to North
    // ------------------------------------------------------------------

    private void initDrivetrainAndImu() {
        fl = hardwareMap.get(DcMotor.class, "fl");
        fr = hardwareMap.get(DcMotor.class, "fr");
        rl = hardwareMap.get(DcMotor.class, "rl");
        rr = hardwareMap.get(DcMotor.class, "rr");

        // Reverted to the directions that actually drive forward
        // correctly (confirmed on the robot). This is NOT the same
        // config as TurnPowerCalibration on fr/rl -- that mismatch is
        // real, but rather than "fix" it here (which broke forward
        // driving), the turning behavior it produced is instead
        // reproduced inside setTurnPower() below by flipping fr/rl's
        // commanded sign there. See the comment on setTurnPower().
        fl.setDirection(DcMotorSimple.Direction.REVERSE);
        rl.setDirection(DcMotorSimple.Direction.REVERSE);
        fr.setDirection(DcMotorSimple.Direction.REVERSE);
        rr.setDirection(DcMotorSimple.Direction.REVERSE);

        fl.setZeroPowerBehavior(DcMotor.ZeroPowerBehavior.BRAKE);
        fr.setZeroPowerBehavior(DcMotor.ZeroPowerBehavior.BRAKE);
        rl.setZeroPowerBehavior(DcMotor.ZeroPowerBehavior.BRAKE);
        rr.setZeroPowerBehavior(DcMotor.ZeroPowerBehavior.BRAKE);

        imu = hardwareMap.get(IMU.class, "imu");
        // Control Hub mounted logo UP, USB BACKWARD, tilted ~36 deg
        // front-to-back (USB edge lower) with a ~6 deg side-to-side lean.
        // Values confirmed empirically with ImuOrientationTest: robot
        // resting flat/level reads Pitch ~0, Roll ~0 with this orientation.
        IMU.Parameters imuParameters = new IMU.Parameters(
                new RevHubOrientationOnRobot(
                        new Orientation(
                                AxesReference.INTRINSIC,
                                AxesOrder.ZYX,
                                AngleUnit.DEGREES,
                                180,   // Z: USB forward -> backward
                                -6,    // Y: side-to-side lean
                                -36,   // X: front-to-back tilt (USB edge down)
                                0)));  // acquisition time
        imu.initialize(imuParameters);
    }

    /**
     * Runs one tick of the Align-to-North state machine and adds
     * telemetry describing its current phase. Called once per loop
     * iteration alongside the existing GPS telemetry.
     */
    private void updateAlignment(JSONObject target, boolean fresh) {
        boolean aPressed = gamepad1.a;
        boolean aEdge = aPressed && !lastAPressed;
        lastAPressed = aPressed;

        switch (alignState) {
            case IDLE:
                if (fresh) {
                    telemetry.addLine("Press A to align to North.");
                    if (aEdge) {
                        startAlignment(target);
                    }
                }
                break;

            case DRIVING: {
                telemetry.addLine("Aligning: driving...");

                // We still need a fresh fix at the *end* of this leg to
                // sample the endpoint for the bearing calc, so a dead
                // GPS link during the drive is still fatal to this
                // attempt even though distance itself no longer depends
                // on GPS.
                if (!fresh) {
                    abortAlignment("GPS fix lost during drive phase.");
                    break;
                }

                long elapsed = System.currentTimeMillis() - alignPhaseStartTime;
                double dist = encoderDisplacementMeters();

                if (dist >= ALIGN_MIN_DISPLACEMENT_M) {
                    setDrivePower(0);
                    double curLat = target.optDouble("cur_lat", 0);
                    double curLon = target.optDouble("cur_lon", 0);
                    alignBearingDeg = computeBearingDeg(alignStartLat, alignStartLon, curLat, curLon);

                    double turnStartYaw = imu.getRobotYawPitchRollAngles().getYaw(AngleUnit.DEGREES);
                    double turnAmount = wrapAngle180(alignBearingDeg);
                    alignTurnTargetYaw = turnStartYaw + turnAmount;

                    alignPhaseStartTime = System.currentTimeMillis();
                    alignState = AlignState.TURNING;
                    telemetry.addData("Aligning: calculating bearing...", "%.1f deg", alignBearingDeg);
                } else if (elapsed > ALIGN_DRIVE_TIMEOUT_MS) {
                    abortAlignment(String.format(
                            "Drive phase timed out before reaching %.1fm displacement (got %.2fm).",
                            ALIGN_MIN_DISPLACEMENT_M, dist));
                } else {
                    telemetry.addData("Displacement (encoders)", "%.2f m / %.2f m", dist, ALIGN_MIN_DISPLACEMENT_M);
                }
                break;
            }

            case TURNING: {
                double currentYaw = imu.getRobotYawPitchRollAngles().getYaw(AngleUnit.DEGREES);
                double error = wrapAngle180(alignTurnTargetYaw - currentYaw);
                long elapsed = System.currentTimeMillis() - alignPhaseStartTime;

                if (Math.abs(error) <= ALIGN_TURN_TOLERANCE_DEG) {
                    setTurnPower(0);
                    imu.resetYaw();
                    alignState = AlignState.DONE;
                    telemetry.addLine("Aligned! IMU zeroed to true north.");
                } else if (elapsed > ALIGN_TURN_TIMEOUT_MS) {
                    abortAlignment(String.format(
                            "Turn-to-north phase timed out (%.1f deg still off).", error));
                } else {
                    double power = error / ALIGN_TURN_FULL_POWER_ERROR_DEG;
                    power = Math.max(-ALIGN_TURN_MAX_POWER, Math.min(ALIGN_TURN_MAX_POWER, power));
                    if (Math.abs(power) < ALIGN_TURN_MIN_POWER) {
                        power = Math.copySign(ALIGN_TURN_MIN_POWER, power);
                    }
                    setTurnPower(power);
                    telemetry.addData("Aligning: turning to north...", "%.1f deg off", error);
                }
                break;
            }

            case DONE:
                telemetry.addLine("Aligned! IMU zeroed to true north.");
                telemetry.addLine("Press A to align again.");
                if (fresh && aEdge) {
                    startAlignment(target);
                }
                break;

            case ABORTED:
                telemetry.addData("Alignment aborted", alignErrorMessage);
                telemetry.addLine("Press A to retry.");
                if (fresh && aEdge) {
                    startAlignment(target);
                }
                break;
        }
    }

    private void startAlignment(JSONObject target) {
        alignStartLat = target.optDouble("cur_lat", 0);
        alignStartLon = target.optDouble("cur_lon", 0);
        alignStartFlTicks = fl.getCurrentPosition();
        alignStartFrTicks = fr.getCurrentPosition();
        alignStartRlTicks = rl.getCurrentPosition();
        alignStartRrTicks = rr.getCurrentPosition();
        alignPhaseStartTime = System.currentTimeMillis();
        alignErrorMessage = "";
        setDrivePower(ALIGN_DRIVE_POWER);
        alignState = AlignState.DRIVING;

        // Starting a fresh alignment invalidates any in-progress or
        // completed waypoint test -- its home point and the IMU zero it
        // relied on are both about to change.
        if (wpState != WaypointState.IDLE) {
            setDrivePower(0);
            wpState = WaypointState.IDLE;
        }
    }

    private void abortAlignment(String reason) {
        setDrivePower(0);
        alignErrorMessage = reason;
        alignState = AlignState.ABORTED;
    }

    /**
     * Average absolute encoder displacement across all four drive
     * motors since the start of the current alignment attempt,
     * converted to meters. Averaging across all four wheels smooths out
     * a bit of per-wheel slip/skew compared to trusting a single wheel.
     */
    private double encoderDisplacementMeters() {
        double flDelta = Math.abs(fl.getCurrentPosition() - alignStartFlTicks);
        double frDelta = Math.abs(fr.getCurrentPosition() - alignStartFrTicks);
        double rlDelta = Math.abs(rl.getCurrentPosition() - alignStartRlTicks);
        double rrDelta = Math.abs(rr.getCurrentPosition() - alignStartRrTicks);
        double avgTicks = (flDelta + frDelta + rlDelta + rrDelta) / 4.0;
        return avgTicks / TICKS_PER_METER;
    }

    private void setDrivePower(double power) {
        fl.setPower(power);
        fr.setPower(power);
        rl.setPower(power);
        rr.setPower(power);
    }

    /**
     * Spins the robot in place. Positive ccwPower turns counterclockwise
     * (right side drives "forward", left side drives "backward"), which
     * matches the convention that CCW rotation increases raw IMU yaw.
     *
     * rl and fr are flipped here relative to fl/rr on purpose. Directions
     * above are set to whatever drives forward correctly, but that config
     * doesn't match the one the turning behavior was confirmed against
     * (which has fr/rl swapped). Flipping their commanded sign here
     * cancels that mismatch out, so the physical rotation this produces
     * matches the turning that was already confirmed to work, without
     * touching the directions that drive forward correctly.
     */
    private void setTurnPower(double ccwPower) {
        fl.setPower(-ccwPower);
        rl.setPower(ccwPower);
        fr.setPower(-ccwPower);
        rr.setPower(ccwPower);
    }

    private static double computeBearingDeg(double lat1, double lon1, double lat2, double lon2) {
        double phi1 = Math.toRadians(lat1);
        double phi2 = Math.toRadians(lat2);
        double dLambda = Math.toRadians(lon2 - lon1);

        double y = Math.sin(dLambda) * Math.cos(phi2);
        double x = Math.cos(phi1) * Math.sin(phi2)
                - Math.sin(phi1) * Math.cos(phi2) * Math.cos(dLambda);
        double theta = Math.atan2(y, x);
        double bearing = Math.toDegrees(theta);
        return (bearing + 360.0) % 360.0;
    }

    /** Wraps an angle in degrees into the range -180..180. */
    private static double wrapAngle180(double deg) {
        double d = deg % 360.0;
        if (d > 180.0) d -= 360.0;
        if (d < -180.0) d += 360.0;
        return d;
    }

    // ------------------------------------------------------------------
    // TCP server (unchanged from Phase 1)
    // ------------------------------------------------------------------

    private void startServer() {
        serverThread = new Thread(() -> {
            try {
                serverSocket = new ServerSocket();
                serverSocket.setReuseAddress(true);
                serverSocket.bind(new InetSocketAddress(PORT));
                connectionState = "listening on :" + PORT;

                while (running.get()) {
                    try {
                        Socket sock = serverSocket.accept();
                        clientSocket = sock;
                        connectionState = "Pi connected";
                        handleClient(sock);
                    } catch (IOException e) {
                        if (running.get()) {
                            connectionState = "accept error: " + e.getMessage();
                        }
                    } finally {
                        if (running.get()) {
                            connectionState = "listening on :" + PORT + " (waiting for reconnect)";
                        }
                    }
                }
            } catch (IOException e) {
                connectionState = "server failed to start: " + e.getMessage();
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
    }

    private void handleClient(Socket sock) {
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(sock.getInputStream(), StandardCharsets.UTF_8));
             OutputStream out = sock.getOutputStream()) {

            String line;
            while (running.get() && (line = reader.readLine()) != null) {
                if (line.trim().isEmpty()) continue;

                try {
                    JSONObject msg = new JSONObject(line);
                    String type = msg.optString("type", "");

                    if ("target".equals(type)) {
                        latestTarget.set(msg);
                        lastMessageAt.set(System.currentTimeMillis());
                        messageCount.incrementAndGet();

                        // Minimal status reply so HubLink.get_status() on
                        // the Pi side has something to show. Phase 1 has
                        // no real dead-reckoning/heading yet, so this just
                        // echoes the position back.
                        JSONObject status = new JSONObject();
                        status.put("type", "status");
                        status.put("state", "driving");
                        status.put("dr_lat", msg.optDouble("cur_lat", 0));
                        status.put("dr_lon", msg.optDouble("cur_lon", 0));
                        status.put("heading_deg", 0);
                        status.put("heading_calibrated", false);
                        status.put("wp_id", msg.optInt("wp_id", 0));
                        status.put("distance_m", 0);
                        status.put("estop", false);
                        status.put("ts", System.currentTimeMillis() / 1000.0);

                        out.write((status.toString() + "\n").getBytes(StandardCharsets.UTF_8));
                        out.flush();

                    } else if ("estop".equals(type) || "clear_estop".equals(type)
                            || "manual".equals(type) || "manual_off".equals(type)) {
                        // Not wired to a drivetrain yet in Phase 1 -- just
                        // read and ignored so the connection stays healthy.
                    }
                } catch (Exception parseError) {
                    // One malformed line shouldn't kill the connection --
                    // same philosophy as hub_link.py's recv loop.
                }
            }
        } catch (IOException e) {
            // Client disconnected or socket error. Fall through and let
            // the accept loop above wait for a reconnect.
        } finally {
            try {
                sock.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void shutdownServer() {
        running.set(false);
        try {
            if (serverSocket != null) serverSocket.close();
        } catch (IOException ignored) {
        }
        try {
            if (clientSocket != null) clientSocket.close();
        } catch (IOException ignored) {
        }
    }
}