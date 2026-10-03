package com.windroid.emu.views;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.HashMap;

/**
 * Dual-layout virtual controller for NFSMW Recompiled.
 *
 * Two switchable layouts, both fully touch-driven and resolution-independent
 * (all geometry is stored normalized to the view size):
 *
 *  - RACING: steering paddles, GAS/BRAKE pedals, NOS, handbrake,
 *    speedbreaker, pause, camera and reset-car buttons.
 *  - GAMEPAD: a full Xbox 360 style pad (dual sticks, D-pad, ABXY,
 *    bumpers/triggers, Start/Back, stick clicks).
 *
 * A switch pill at the top-center toggles layouts at any time while playing.
 * Long-press the switch pill to enter EDIT mode: drag any control to move it,
 * long-press again to save and exit. Per-layout scale and opacity come from
 * the launcher settings (and can be edited there); custom positions are
 * persisted per layout.
 */
public class VirtualControllerInputView extends View {

    public final static int SHAPE_CIRCLE = 0;
    public final static int SHAPE_SQUARE = 1;
    public final static int SHAPE_RECTANGLE = 2;
    public final static int SHAPE_DPAD = 3;
    public final static int SHAPE_PILL = 4;

    public static final int UP = 1;
    public static final int RIGHT_UP = 2;
    public static final int RIGHT = 3;
    public static final int RIGHT_DOWN = 4;
    public static final int DOWN = 5;
    public static final int LEFT_DOWN = 6;
    public static final int LEFT = 7;
    public static final int LEFT_UP = 8;

    public final static int A_BUTTON = 1;
    public final static int B_BUTTON = 2;
    public final static int X_BUTTON = 3;
    public final static int Y_BUTTON = 4;
    public final static int START_BUTTON = 5;
    public final static int SELECT_BUTTON = 6;
    public final static int LB_BUTTON = 7;
    public final static int LT_BUTTON = 8;
    public final static int RB_BUTTON = 9;
    public final static int RT_BUTTON = 10;
    public final static int LEFT_ANALOG = 11;
    public final static int LS_BUTTON = 12;
    public final static int RS_BUTTON = 13;
    public final static int RIGHT_ANALOG = 14;
    public final static int DPAD_CONTROL = 15;

    // Racing-layout control ids
    public final static int STEER_L = 101;
    public final static int STEER_R = 102;
    public final static int GAS_PEDAL = 103;
    public final static int BRAKE_PEDAL = 104;
    public final static int NITRO = 105;
    public final static int HANDBRAKE = 106;
    public final static int SPEEDBREAKER = 107;
    public final static int PAUSE_BTN = 108;
    public final static int CAMERA_BTN = 109;
    public final static int RESET_BTN = 110;

    public final static int SWITCH_BTN = 0;

    // Button state bitmasks matching GameActivity's XInput mapping
    public final static int MASK_A      = 0x01;
    public final static int MASK_B      = 0x02;
    public final static int MASK_X      = 0x04;
    public final static int MASK_Y      = 0x08;
    public final static int MASK_RB     = 0x10;
    public final static int MASK_LB     = 0x20;
    public final static int MASK_LS     = 0x40;
    public final static int MASK_RS     = 0x80;

    public final static int MASK_START  = 0x01;
    public final static int MASK_SELECT = 0x02;

    public static final String MODE_GAMEPAD = "gamepad";
    public static final String MODE_RACING = "racing";

    private static final String PREFS_NAME = "virtual_controller_windroid";
    private static final long LONG_PRESS_MS = 600;

    public interface ControllerListener {
        void onControllerState(float lx, float ly, float rx, float ry, float lt, float rt,
                               int buttonsA, int buttonsB, int dpadStatus);
    }

    private ControllerListener listener;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rectF = new RectF();

    private final Path dpadUp = new Path();
    private final Path dpadDown = new Path();
    private final Path dpadLeft = new Path();
    private final Path dpadRight = new Path();

    private final ArrayList<VCControl> controls = new ArrayList<>();
    private final HashMap<Integer, VCControl> controlById = new HashMap<>();

    private final SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private String mode = MODE_GAMEPAD;
    private float layoutScale = 1.0F;
    private float layoutAlpha = 0.85F;

    private boolean isEditing = false;
    private boolean dragging = false;
    private VCControl dragControl = null;
    private int dragPointerId = -1;
    private float dragOffsetX = 0F;
    private float dragOffsetY = 0F;
    private boolean switchPressPendingToggle = false;

    private byte buttonsStateA = 0;
    private byte buttonsStateB = 0;
    private float lt = 0F;
    private float rt = 0F;

    public VirtualControllerInputView(Context context) {
        super(context);
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        init();
    }

    public VirtualControllerInputView(Context context, AttributeSet attrs) {
        super(context, attrs);
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        init();
    }

    public VirtualControllerInputView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        init();
    }

    public void setControllerListener(ControllerListener listener) {
        this.listener = listener;
    }

    /** Current layout mode ("gamepad" or "racing"). */
    public String getMode() {
        return mode;
    }

    private void init() {
        setClickable(true);
        setFocusable(true);
        setFocusableInTouchMode(true);

        paint.setStrokeWidth(6F);
        paint.setColor(Color.WHITE);
        paint.setStyle(Paint.Style.STROKE);

        fillPaint.setStyle(Paint.Style.FILL);

        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));

        mode = prefs.getString("ctl_mode", MODE_GAMEPAD);
        rebuildLayout();
    }

    // ------------------------------------------------------------------
    //  Layout construction
    // ------------------------------------------------------------------

    private void rebuildLayout() {
        releaseAllStates();
        layoutScale = clamp(prefs.getFloat("ctl_" + mode + "_scale", 1.0F), 0.6F, 1.6F);
        layoutAlpha = clamp(prefs.getFloat("ctl_" + mode + "_alpha", 0.85F), 0.25F, 1.0F);

        controls.clear();
        controlById.clear();

        // Mode switch pill (top-center, fixed, not user-movable).
        addControl(SWITCH_BTN, 0.5F, 0.062F, 0.0F, SHAPE_PILL, "");

        if (MODE_RACING.equals(mode)) {
            addControl(STEER_L,      0.105F, 0.735F, 0.135F, SHAPE_CIRCLE,    "◀");
            addControl(STEER_R,      0.280F, 0.735F, 0.135F, SHAPE_CIRCLE,    "▶");
            addControl(BRAKE_PEDAL,  0.735F, 0.775F, 0.100F, SHAPE_CIRCLE,    "BRK");
            addControl(GAS_PEDAL,    0.915F, 0.775F, 0.100F, SHAPE_CIRCLE,    "GAS");
            addControl(NITRO,        0.915F, 0.440F, 0.075F, SHAPE_CIRCLE,    "NOS");
            addControl(HANDBRAKE,    0.735F, 0.440F, 0.075F, SHAPE_CIRCLE,    "HB");
            addControl(SPEEDBREAKER, 0.585F, 0.600F, 0.068F, SHAPE_CIRCLE,    "SB");
            addControl(PAUSE_BTN,    0.945F, 0.115F, 0.058F, SHAPE_CIRCLE,    "II");
            addControl(CAMERA_BTN,   0.810F, 0.115F, 0.058F, SHAPE_CIRCLE,    "CAM");
            addControl(RESET_BTN,    0.055F, 0.115F, 0.058F, SHAPE_CIRCLE,    "RST");
        } else {
            addControl(A_BUTTON,      0.860F, 0.843F, 0.075F, SHAPE_CIRCLE,    "A");
            addControl(B_BUTTON,      0.919F, 0.681F, 0.075F, SHAPE_CIRCLE,    "B");
            addControl(X_BUTTON,      0.802F, 0.681F, 0.075F, SHAPE_CIRCLE,    "X");
            addControl(Y_BUTTON,      0.860F, 0.519F, 0.075F, SHAPE_CIRCLE,    "Y");
            addControl(START_BUTTON,  0.554F, 0.907F, 0.054F, SHAPE_CIRCLE,    "");
            addControl(SELECT_BUTTON, 0.467F, 0.907F, 0.054F, SHAPE_CIRCLE,    "");
            addControl(LB_BUTTON,     0.117F, 0.278F, 0.108F, SHAPE_RECTANGLE, "LB");
            addControl(LT_BUTTON,     0.117F, 0.130F, 0.108F, SHAPE_RECTANGLE, "LT");
            addControl(RB_BUTTON,     0.860F, 0.278F, 0.108F, SHAPE_RECTANGLE, "RB");
            addControl(RT_BUTTON,     0.860F, 0.130F, 0.108F, SHAPE_RECTANGLE, "RT");
            addControl(LS_BUTTON,     0.240F, 0.560F, 0.075F, SHAPE_CIRCLE,    "LS");
            addControl(RS_BUTTON,     0.640F, 0.560F, 0.075F, SHAPE_CIRCLE,    "RS");
            addControl(LEFT_ANALOG,   0.117F, 0.778F, 0.115F, SHAPE_CIRCLE,    "");
            addControl(RIGHT_ANALOG,  0.729F, 0.560F, 0.115F, SHAPE_CIRCLE,    "");
            addControl(DPAD_CONTROL,  0.267F, 0.444F, 0.083F, SHAPE_DPAD,      "");
        }

        // Restore user-moved positions (normalized, per layout).
        for (VCControl c : controls) {
            if (c.id == SWITCH_BTN) continue;
            if (prefs.getBoolean("pos_" + mode + "_" + c.id + "_set", false)) {
                c.nx = clamp(prefs.getFloat("pos_" + mode + "_" + c.id + "_x", c.nx), 0.03F, 0.97F);
                c.ny = clamp(prefs.getFloat("pos_" + mode + "_" + c.id + "_y", c.ny), 0.06F, 0.94F);
            }
        }

        applyGeometry();
        invalidate();
    }

    private void addControl(int id, float nx, float ny, float nradius, int shape, String label) {
        VCControl c = new VCControl(id, nx, ny, nradius, shape, label);
        controls.add(c);
        controlById.put(id, c);
    }

    /** Recomputes pixel geometry from normalized coordinates. */
    private void applyGeometry() {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float scaleBase = Math.min(w, h * 2.2F); // keeps buttons sane on very tall screens
        for (VCControl c : controls) {
            c.x = c.nx * w;
            c.y = c.ny * h;
            c.radius = (c.id == SWITCH_BTN ? 0.040F : c.nradius) * scaleBase * layoutScale;
        }
        textPaint.setTextSize(Math.max(22F, 0.024F * scaleBase * layoutScale));
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        applyGeometry();
        invalidate();
    }

    private static float clamp(float v, float min, float max) {
        return v < min ? min : Math.min(v, max);
    }

    // ------------------------------------------------------------------
    //  Input state
    // ------------------------------------------------------------------

    private void handleButton(VCControl c, boolean isPressed) {
        c.isPressed = isPressed;
        switch (c.id) {
            // Gamepad face / shoulders / clicks
            case A_BUTTON:      setMask(isPressed, MASK_A); return;
            case B_BUTTON:      setMask(isPressed, MASK_B); return;
            case X_BUTTON:      setMask(isPressed, MASK_X); return;
            case Y_BUTTON:      setMask(isPressed, MASK_Y); return;
            case LB_BUTTON:     setMask(isPressed, MASK_LB); return;
            case RB_BUTTON:     setMask(isPressed, MASK_RB); return;
            case LS_BUTTON:     setMask(isPressed, MASK_LS); return;
            case RS_BUTTON:     setMask(isPressed, MASK_RS); return;
            case LT_BUTTON:     lt = isPressed ? 1F : 0F; return;
            case RT_BUTTON:     rt = isPressed ? 1F : 0F; return;
            case START_BUTTON:  setMaskB(isPressed, MASK_START); return;
            case SELECT_BUTTON: setMaskB(isPressed, MASK_SELECT); return;

            // Racing layout mapping (mirrors the engine's racing overlay)
            case GAS_PEDAL:     rt = isPressed ? 1F : 0F; return;
            case BRAKE_PEDAL:   lt = isPressed ? 1F : 0F; return;
            case NITRO:         setMask(isPressed, MASK_A); return;
            case HANDBRAKE:     setMask(isPressed, MASK_B); return;
            case SPEEDBREAKER:  setMask(isPressed, MASK_LS); return;
            case PAUSE_BTN:     setMaskB(isPressed, MASK_START); return;
            case RESET_BTN:     setMaskB(isPressed, MASK_SELECT); return;
            case CAMERA_BTN:    setMask(isPressed, MASK_RB); return;
            default: return;
        }
    }

    private void setMask(boolean isPressed, int mask) {
        if (isPressed) buttonsStateA |= mask;
        else buttonsStateA &= ~mask;
    }

    private void setMaskB(boolean isPressed, int mask) {
        if (isPressed) buttonsStateB |= mask;
        else buttonsStateB &= ~mask;
    }

    private void releaseAllStates() {
        for (VCControl c : controls) {
            c.isPressed = false;
            c.fingerId = -1;
            c.fingerX = 0F;
            c.fingerY = 0F;
            c.dpadStatus = 0;
        }
        buttonsStateA = 0;
        buttonsStateB = 0;
        lt = 0F;
        rt = 0F;
        emitState();
    }

    private void emitState() {
        if (listener == null) return;

        float lx = 0F, ly = 0F, rx = 0F, ry = 0F;
        byte dpadStatus = 0;

        VCControl left = controlById.get(LEFT_ANALOG);
        VCControl right = controlById.get(RIGHT_ANALOG);
        VCControl dpad = controlById.get(DPAD_CONTROL);

        if (left != null && left.isPressed) {
            lx = clamp(left.fingerX / (left.radius / 4F), -1F, 1F);
            ly = clamp(left.fingerY / (left.radius / 4F), -1F, 1F);
        }
        if (right != null && right.isPressed) {
            rx = clamp(right.fingerX / (right.radius / 4F), -1F, 1F);
            ry = clamp(right.fingerY / (right.radius / 4F), -1F, 1F);
        }

        // Racing steering paddles drive the left stick X axis.
        VCControl steerL = controlById.get(STEER_L);
        VCControl steerR = controlById.get(STEER_R);
        if (steerL != null && steerL.isPressed) lx = -1F;
        if (steerR != null && steerR.isPressed) lx = 1F;

        if (dpad != null) dpadStatus = (byte) dpad.dpadStatus;

        listener.onControllerState(lx, ly, rx, ry, lt, rt, buttonsStateA, buttonsStateB, dpadStatus);
    }

    // ------------------------------------------------------------------
    //  Layout switching + editing
    // ------------------------------------------------------------------

    private void toggleMode() {
        mode = MODE_RACING.equals(mode) ? MODE_GAMEPAD : MODE_RACING;
        prefs.edit().putString("ctl_mode", mode).apply();
        rebuildLayout();
    }

    private void toggleEdit() {
        isEditing = !isEditing;
        if (!isEditing && dragControl != null) {
            savePosition(dragControl);
            dragControl = null;
            dragging = false;
        }
        releaseAllStates();
        invalidate();
    }

    private void savePosition(VCControl c) {
        prefs.edit()
                .putBoolean("pos_" + mode + "_" + c.id + "_set", true)
                .putFloat("pos_" + mode + "_" + c.id + "_x", c.nx)
                .putFloat("pos_" + mode + "_" + c.id + "_y", c.ny)
                .apply();
    }

    /** Clears custom positions and scale/opacity overrides for both layouts. */
    public void resetLayouts() {
        SharedPreferences.Editor e = prefs.edit();
        for (String m : new String[] {MODE_GAMEPAD, MODE_RACING}) {
            e.remove("ctl_" + m + "_scale").remove("ctl_" + m + "_alpha");
            for (int id : new int[] {A_BUTTON, B_BUTTON, X_BUTTON, Y_BUTTON, START_BUTTON,
                    SELECT_BUTTON, LB_BUTTON, LT_BUTTON, RB_BUTTON, RT_BUTTON, LEFT_ANALOG,
                    LS_BUTTON, RS_BUTTON, RIGHT_ANALOG, DPAD_CONTROL, STEER_L, STEER_R,
                    GAS_PEDAL, BRAKE_PEDAL, NITRO, HANDBRAKE, SPEEDBREAKER, PAUSE_BTN,
                    CAMERA_BTN, RESET_BTN}) {
                e.remove("pos_" + m + "_" + id + "_set")
                        .remove("pos_" + m + "_" + id + "_x")
                        .remove("pos_" + m + "_" + id + "_y");
            }
        }
        e.apply();
        rebuildLayout();
    }

    // ------------------------------------------------------------------
    //  Touch handling
    // ------------------------------------------------------------------

    private static boolean hitTest(MotionEvent event, int index, VCControl c) {
        float ex = event.getX(index);
        float ey = event.getY(index);
        switch (c.shape) {
            case SHAPE_RECTANGLE:
                return (ex >= c.x - c.radius * 0.65F && ex <= c.x + c.radius * 0.65F) &&
                        (ey >= c.y - c.radius * 0.40F && ey <= c.y + c.radius * 0.40F);
            case SHAPE_DPAD:
                return (ex >= c.x - c.radius - 30F && ex <= c.x + c.radius + 30F) &&
                        (ey >= c.y - c.radius - 30F && ey <= c.y + c.radius + 30F);
            case SHAPE_PILL:
                return ex >= c.x - c.radius * 1.4F && ex <= c.x + c.radius * 1.4F &&
                        ey >= c.y - c.radius * 0.6F && ey <= c.y + c.radius * 0.6F;
            default:
                float dx = ex - c.x;
                float dy = ey - c.y;
                float r = c.radius * 0.65F;
                return dx * dx + dy * dy <= r * r;
        }
    }

    private static int getAxisStatus(float axisX, float axisY, float deadZone) {
        boolean xNeutral = (axisX < deadZone && axisX > -deadZone);
        boolean yNeutral = (axisY < deadZone && axisY > -deadZone);

        if (axisX > deadZone && axisY < -deadZone) return RIGHT_UP;
        if (axisX > deadZone && yNeutral) return RIGHT;
        if (axisX > deadZone && axisY > deadZone) return RIGHT_DOWN;
        if (axisY > deadZone && xNeutral) return DOWN;
        if (axisY < -deadZone && xNeutral) return UP;
        if (axisX < -deadZone && axisY > deadZone) return LEFT_DOWN;
        if (axisX < -deadZone && yNeutral) return LEFT;
        if (axisX < -deadZone && axisY < -deadZone) return LEFT_UP;
        return 0;
    }

    private void moveStick(VCControl stick, float ex, float ey) {
        float posX = ex - stick.x;
        float posY = ey - stick.y;
        float maxDist = stick.radius / 4F;
        float dist = (float) Math.sqrt(posX * posX + posY * posY);
        if (dist > maxDist) {
            float s = maxDist / dist;
            posX *= s;
            posY *= s;
        }
        stick.fingerX = posX;
        stick.fingerY = posY;
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int idx = event.getActionIndex();

        switch (action) {
            case MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                // Switch pill: tap = toggle layout, long-press = edit mode.
                VCControl sw = controlById.get(SWITCH_BTN);
                if (sw != null && hitTest(event, idx, sw)) {
                    sw.isPressed = true;
                    sw.fingerId = event.getPointerId(idx);
                    switchPressPendingToggle = true;
                    handler.postDelayed(() -> {
                        if (sw.isPressed) {
                            switchPressPendingToggle = false;
                            toggleEdit();
                        }
                    }, LONG_PRESS_MS);
                    invalidate();
                    return true;
                }

                if (isEditing) {
                    for (VCControl c : controls) {
                        if (c.id == SWITCH_BTN) continue;
                        if (hitTest(event, idx, c)) {
                            dragControl = c;
                            dragging = true;
                            dragPointerId = event.getPointerId(idx);
                            dragOffsetX = c.x - event.getX(idx);
                            dragOffsetY = c.y - event.getY(idx);
                            break;
                        }
                    }
                    invalidate();
                    return true;
                }

                for (VCControl c : controls) {
                    if (c.id == SWITCH_BTN) continue;
                    if (!hitTest(event, idx, c)) continue;

                    c.fingerId = event.getPointerId(idx);

                    if (c.shape == SHAPE_CIRCLE && (c.id == LEFT_ANALOG || c.id == RIGHT_ANALOG)) {
                        c.isPressed = true;
                        moveStick(c, event.getX(idx), event.getY(idx));
                    } else if (c.shape == SHAPE_DPAD) {
                        c.isPressed = true;
                        c.fingerX = event.getX(idx) - c.x;
                        c.fingerY = event.getY(idx) - c.y;
                        c.dpadStatus = getAxisStatus(c.fingerX / c.radius, c.fingerY / c.radius, 0.25F);
                    } else {
                        handleButton(c, true);
                    }
                    break;
                }
                invalidate();
            }
            case MotionEvent.ACTION_MOVE -> {
                if (dragging && dragControl != null && isEditing) {
                    int di = event.findPointerIndex(dragPointerId);
                    if (di >= 0) {
                        int w = getWidth();
                        int h = getHeight();
                        dragControl.nx = clamp((event.getX(di) + dragOffsetX) / w, 0.03F, 0.97F);
                        dragControl.ny = clamp((event.getY(di) + dragOffsetY) / h, 0.06F, 0.94F);
                        applyGeometry();
                    }
                    invalidate();
                    return true;
                }
                if (!isEditing) {
                    for (int i = 0; i < event.getPointerCount(); i++) {
                        int pid = event.getPointerId(i);
                        for (VCControl c : controls) {
                            if (c.fingerId != pid) continue;
                            if (c.id == LEFT_ANALOG || c.id == RIGHT_ANALOG) {
                                moveStick(c, event.getX(i), event.getY(i));
                            } else if (c.shape == SHAPE_DPAD) {
                                c.fingerX = event.getX(i) - c.x;
                                c.fingerY = event.getY(i) - c.y;
                                c.dpadStatus = getAxisStatus(c.fingerX / c.radius, c.fingerY / c.radius, 0.25F);
                            }
                        }
                    }
                }
                invalidate();
            }
            case MotionEvent.ACTION_POINTER_UP -> {
                int pid = event.getPointerId(idx);

                VCControl sw = controlById.get(SWITCH_BTN);
                if (sw != null && sw.fingerId == pid) {
                    sw.fingerId = -1;
                    sw.isPressed = false;
                    if (switchPressPendingToggle) {
                        switchPressPendingToggle = false;
                        toggleMode();
                    }
                    invalidate();
                    return true;
                }

                for (VCControl c : controls) {
                    if (c.fingerId == pid) {
                        c.fingerId = -1;
                        if (c.id == LEFT_ANALOG || c.id == RIGHT_ANALOG) {
                            c.isPressed = false;
                            c.fingerX = 0F;
                            c.fingerY = 0F;
                        } else if (c.shape == SHAPE_DPAD) {
                            c.isPressed = false;
                            c.fingerX = 0F;
                            c.fingerY = 0F;
                            c.dpadStatus = 0;
                        } else {
                            handleButton(c, false);
                        }
                    }
                }
                invalidate();
            }
            case MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                VCControl sw = controlById.get(SWITCH_BTN);
                if (sw != null && sw.isPressed) {
                    sw.fingerId = -1;
                    sw.isPressed = false;
                    if (switchPressPendingToggle) {
                        switchPressPendingToggle = false;
                        toggleMode();
                    }
                }
                if (dragging && dragControl != null) {
                    savePosition(dragControl);
                    dragControl = null;
                    dragging = false;
                }
                releaseAllStates();
                invalidate();
            }
        }

        emitState();
        return true;
    }

    // ------------------------------------------------------------------
    //  Drawing
    // ------------------------------------------------------------------

    private int alpha(int base) {
        return (int) (base * layoutAlpha);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        // Controls
        for (VCControl c : controls) {
            if (c.id == SWITCH_BTN) continue;

            boolean pressed = c.isPressed;
            paint.setStyle(pressed || (isEditing && c == dragControl)
                    ? Paint.Style.FILL_AND_STROKE : Paint.Style.STROKE);
            paint.setColor(Color.WHITE);
            paint.setStrokeWidth(6F);
            paint.setAlpha(alpha(isEditing ? 255 : 210));

            boolean hasLabel = c.label != null && !c.label.isEmpty();
            textPaint.setColor(pressed ? Color.BLACK : Color.WHITE);
            textPaint.setAlpha(alpha(isEditing ? 255 : 235));

            float offset = (textPaint.getFontMetrics().ascent + textPaint.getFontMetrics().descent) / 2F;

            if (c.shape == SHAPE_DPAD) {
                drawDPadArrows(canvas, c);
                continue;
            }

            if (c.id == LEFT_ANALOG || c.id == RIGHT_ANALOG) {
                // Stick base + knob
                paint.setStyle(Paint.Style.STROKE);
                paint.setAlpha(alpha(isEditing ? 255 : 200));
                paint.setStrokeWidth(8F);
                canvas.drawCircle(c.x, c.y, c.radius / 2F, paint);

                float knobX = c.x + c.fingerX;
                float knobY = c.y + c.fingerY;
                float maxDist = c.radius / 4F;
                float distSq = c.fingerX * c.fingerX + c.fingerY * c.fingerY;
                if (distSq > maxDist * maxDist) {
                    float dist = (float) Math.sqrt(distSq);
                    knobX = c.x + c.fingerX * (maxDist / dist);
                    knobY = c.y + c.fingerY * (maxDist / dist);
                }
                fillPaint.setColor(Color.WHITE);
                fillPaint.setAlpha(alpha(isEditing ? 255 : 200));
                canvas.drawCircle(knobX, knobY, c.radius / 4F, fillPaint);
                continue;
            }

            if (c.shape == SHAPE_RECTANGLE) {
                rectF.set(c.x - c.radius / 2F, c.y - c.radius / 4F,
                        c.x + c.radius / 2F, c.y + c.radius / 4F);
                canvas.drawRoundRect(rectF, 24F, 24F, paint);
            } else {
                canvas.drawCircle(c.x, c.y, c.radius / 2F, paint);
            }

            if (hasLabel) {
                canvas.drawText(c.label, c.x, c.y - offset, textPaint);
            }

            if (c.id == START_BUTTON) {
                drawStartGlyph(canvas, c);
            } else if (c.id == SELECT_BUTTON) {
                drawSelectGlyph(canvas, c);
            }
        }

        drawSwitchPill(canvas);

        if (isEditing) {
            textPaint.setColor(Color.YELLOW);
            textPaint.setAlpha(255);
            float w = getWidth();
            canvas.drawText("EDIT MODE - drag controls, long-press switch to finish",
                    w / 2F, textPaint.getTextSize() * 3.6F, textPaint);
        }
    }

    private void drawSwitchPill(Canvas canvas) {
        VCControl sw = controlById.get(SWITCH_BTN);
        if (sw == null) return;

        String label = MODE_RACING.equals(mode) ? "RACE" : "PAD";
        paint.setStyle(sw.isPressed || isEditing ? Paint.Style.FILL_AND_STROKE : Paint.Style.STROKE);
        paint.setColor(isEditing ? Color.YELLOW : Color.WHITE);
        paint.setStrokeWidth(4F);
        paint.setAlpha(alpha(isEditing ? 255 : 200));

        rectF.set(sw.x - sw.radius * 1.35F, sw.y - sw.radius * 0.55F,
                sw.x + sw.radius * 1.35F, sw.y + sw.radius * 0.55F);
        canvas.drawRoundRect(rectF, sw.radius * 0.55F, sw.radius * 0.55F, paint);

        textPaint.setColor(sw.isPressed ? Color.BLACK : (isEditing ? Color.YELLOW : Color.WHITE));
        textPaint.setAlpha(alpha(isEditing ? 255 : 240));
        float offset = (textPaint.getFontMetrics().ascent + textPaint.getFontMetrics().descent) / 2F;
        canvas.drawText("\u21C4 " + label, sw.x, sw.y - offset, textPaint);
    }

    private void drawStartGlyph(Canvas canvas, VCControl c) {
        paint.setStrokeWidth(4F);
        paint.setColor(c.isPressed ? Color.BLACK : Color.WHITE);
        paint.setAlpha(alpha(isEditing ? 255 : 230));
        float w = c.radius / 3F;
        for (int i = -1; i <= 1; i++) {
            float yy = c.y + i * c.radius / 7F;
            canvas.drawLine(c.x - w / 2F, yy, c.x + w / 2F, yy, paint);
        }
    }

    private void drawSelectGlyph(Canvas canvas, VCControl c) {
        paint.setStrokeWidth(4F);
        paint.setColor(c.isPressed ? Color.BLACK : Color.WHITE);
        paint.setAlpha(alpha(isEditing ? 255 : 230));
        // Two overlapping rounded rectangles (the 360 "back" glyphs)
        float s = c.radius / 5F;
        rectF.set(c.x - s * 1.4F, c.y - s * 0.8F, c.x - s * 0.4F, c.y + s * 0.8F);
        canvas.drawRoundRect(rectF, 3F, 3F, paint);
        rectF.set(c.x + s * 0.4F, c.y - s * 0.8F, c.x + s * 1.4F, c.y + s * 0.8F);
        canvas.drawRoundRect(rectF, 3F, 3F, paint);
    }

    private void drawDPadArrows(Canvas canvas, VCControl c) {
        float r = c.radius;
        dpadLeft.reset();
        dpadLeft.moveTo(c.x - 10F, c.y);
        dpadLeft.lineTo(c.x - 10F - r / 4F, c.y - r / 4F);
        dpadLeft.lineTo(c.x - 10F - r / 4F - r / 2F, c.y - r / 4F);
        dpadLeft.lineTo(c.x - 10F - r / 4F - r / 2F, c.y - r / 4F + r / 2F);
        dpadLeft.lineTo(c.x - 10F - r / 4F, c.y - r / 4F + r / 2F);
        dpadLeft.lineTo(c.x - 10F, c.y);
        dpadLeft.close();

        dpadRight.reset();
        dpadRight.moveTo(c.x + 10F, c.y);
        dpadRight.lineTo(c.x + 10F + r / 4F, c.y - r / 4F);
        dpadRight.lineTo(c.x + 10F + r / 4F + r / 2F, c.y - r / 4F);
        dpadRight.lineTo(c.x + 10F + r / 4F + r / 2F, c.y - r / 4F + r / 2F);
        dpadRight.lineTo(c.x + 10F + r / 4F, c.y - r / 4F + r / 2F);
        dpadRight.lineTo(c.x + 10F, c.y);
        dpadRight.close();

        dpadUp.reset();
        dpadUp.moveTo(c.x, c.y - 10F);
        dpadUp.lineTo(c.x - r / 4F, c.y - 10F - r / 4F);
        dpadUp.lineTo(c.x - r / 4F, c.y - 10F - r / 4F - r / 2F);
        dpadUp.lineTo(c.x - r / 4F + r / 2F, c.y - 10F - r / 4F - r / 2F);
        dpadUp.lineTo(c.x - r / 4F + r / 2F, c.y - 10F - r / 4F);
        dpadUp.lineTo(c.x, c.y - 10F);
        dpadUp.close();

        dpadDown.reset();
        dpadDown.moveTo(c.x, c.y + 10F);
        dpadDown.lineTo(c.x - r / 4F, c.y + 10F + r / 4F);
        dpadDown.lineTo(c.x - r / 4F, c.y + 10F + r / 4F + r / 2F);
        dpadDown.lineTo(c.x - r / 4F + r / 2F, c.y + 10F + r / 4F + r / 2F);
        dpadDown.lineTo(c.x - r / 4F + r / 2F, c.y + 10F + r / 4F);
        dpadDown.lineTo(c.x, c.y + 10F);
        dpadDown.close();

        paint.setStrokeWidth(4F);
        paint.setAlpha(alpha(isEditing ? 255 : 200));
        int stat = c.dpadStatus;
        drawArrow(canvas, dpadUp,    stat == UP || stat == RIGHT_UP || stat == LEFT_UP);
        drawArrow(canvas, dpadDown,  stat == DOWN || stat == RIGHT_DOWN || stat == LEFT_DOWN);
        drawArrow(canvas, dpadLeft,  stat == LEFT || stat == LEFT_DOWN || stat == LEFT_UP);
        drawArrow(canvas, dpadRight, stat == RIGHT || stat == RIGHT_DOWN || stat == RIGHT_UP);
    }

    private void drawArrow(Canvas canvas, Path path, boolean pressed) {
        paint.setStyle(pressed ? Paint.Style.FILL_AND_STROKE : Paint.Style.STROKE);
        paint.setColor(Color.WHITE);
        canvas.drawPath(path, paint);
    }

    // ------------------------------------------------------------------
    //  Control model
    // ------------------------------------------------------------------

    public static class VCControl {
        public final int id;
        public final int shape;
        public final String label;
        public float nx;      // normalized center X (0..1 of view width)
        public float ny;      // normalized center Y (0..1 of view height)
        public float nradius; // normalized radius (fraction of view height)
        public float x;
        public float y;
        public float radius;
        public int fingerId = -1;
        public boolean isPressed = false;
        public float fingerX = 0F;
        public float fingerY = 0F;
        public int dpadStatus = 0;

        public VCControl(int id, float nx, float ny, float nradius, int shape, String label) {
            this.id = id;
            this.nx = nx;
            this.ny = ny;
            this.nradius = nradius;
            this.shape = shape;
            this.label = label;
        }
    }
}
