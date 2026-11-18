import totk from 'libtotk_ohos.so';   // the native runtime (native-harmonyos/ohos/frontend_ohos.c)
import { readTouchTimestamp } from './EventClock';

// Relative drag uses the standard right-stick HID path (distance-following,
// frontend_ohos.c addTouchLook). It does not read or write version-specific
// camera objects; lifting, cancelling or changing modes releases the pointer.
@Component
export struct SwipeLook {
  @Prop @Watch('resetControl') areaWidth: number = 0;
  @Prop @Watch('resetControl') areaHeight: number = 0;
  @Prop @Watch('resetControl') controlEnabled: boolean = true;
  @Prop @Watch('resetControl') resetVersion: number = 0;
  private pointer: number = -1;
  private epoch: number = 0;
  private downTimestamp: number = 0;
  private lastTimestamp: number = 0;
  private lastClock: number = 0;
  private lastX: number = 0;
  private lastY: number = 0;
  private releaseTimer: number = -1;
  private revision: number = 0;

  private resetControl(): void {
    this.revision += 1;
    if (this.releaseTimer >= 0) clearTimeout(this.releaseTimer);
    this.releaseTimer = -1;
    if (this.pointer >= 0) totk.releaseTouchPointer(this.pointer, this.epoch);
    this.pointer = -1;
    this.lastClock = 0;
  }

  aboutToDisappear(): void { this.resetControl(); }

  private begin(touch: TouchObject, timestamp: number): void {
    this.pointer = touch.id;
    this.epoch = totk.getTouchInputEpoch();
    this.downTimestamp = timestamp;
    this.lastTimestamp = timestamp;
    this.lastClock = Date.now();
    this.lastX = touch.x;
    this.lastY = touch.y;
  }

  private move(touch: TouchObject, timestamp: number): void {
    if (timestamp < this.lastTimestamp || !Number.isFinite(touch.x) || !Number.isFinite(touch.y)) return;
    // The finger's distance, in fractions of the area's height (the same physical gesture on every phone). The runtime
    // (frontend_ohos.c addTouchLook) turns it into right-stick input as the game reads the pad, so the camera follows
    // the finger and stops in the frame after it stops; a stick velocity from here lagged and dropped short swipes.
    const h = Math.max(200, this.areaHeight);
    const dx = (touch.x - this.lastX) / h;
    const dy = (this.lastY - touch.y) / h;
    this.lastClock = Date.now();
    this.lastTimestamp = timestamp;
    this.lastX = touch.x;
    this.lastY = touch.y;
    if ((dx !== 0 || dy !== 0) && !totk.addTouchLook(this.pointer, dx, dy, this.epoch)) this.resetControl();
  }

  private touch(event: TouchEvent): void {
    const timestamp = readTouchTimestamp(event);
    if (!Number.isFinite(timestamp)) return;
    if (event.type === TouchType.Cancel) {
      if (timestamp >= this.downTimestamp) this.resetControl();
      return;
    }
    event.changedTouches.forEach((touch: TouchObject) => {
      if (event.type === TouchType.Down && this.controlEnabled && this.pointer < 0 &&
        Number.isFinite(touch.x) && Number.isFinite(touch.y)) {
        this.begin(touch, timestamp);
      } else if (touch.id === this.pointer && timestamp >= this.downTimestamp) {
        if (event.type === TouchType.Move) this.move(touch, timestamp);
        else if (event.type === TouchType.Up) this.resetControl();
      }
    });
  }

  build() {
    Stack() {}
      .width(this.areaWidth).height(this.areaHeight)
      .backgroundColor(Color.Transparent)
      .hitTestBehavior(HitTestMode.Block)
      .onTouch((event: TouchEvent) => { this.touch(event); })
      .accessibilityText('划屏视角区域')
      .accessibilityDescription('在右侧空白区域拖动调整视角，停止拖动或松开后停止转动')
  }
}
