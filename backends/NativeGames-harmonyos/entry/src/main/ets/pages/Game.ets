// The Android port's native stack (native-harmonyos/, libtotk_ohos.so) in a full-screen XComponent: the runtime starts
// when the page appears, and the surface goes to its Vulkan presenter (native-harmonyos/ohos/frontend_ohos.c) as soon as
// its size is known. Until the game shows its first picture the loading screen covers it (bootStatus()). The on-screen
// controls (components/Controls) and any physical controller (the runtime reads them through GameControllerKit) feed
// the game's controller; the game menu (the small tab at the top centre, or the back gesture) continues, opens the
// settings, sets the on-screen keys (auto: hidden while a controller is connected) and the camera control, or goes
// back to the start page. The game's keyboard requests open a text dialog here.
// Launch from a PC: aa start ... --ps view game. Log: files/android/totk-native.log.
import totk from 'libtotk_ohos.so';
import { TextRequest, GamepadStatus, BootStatus } from 'libtotk_ohos.so';
import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PhoneControls } from '../components/Controls/PhoneControls';
import { TotkSettings } from '../components/Settings/SettingsModel';
import { SettingsPanel } from '../components/Settings/SettingsPanel';
import { LoadingOverlay, BootTracker, FRAMES_TO_SHOW } from '../components/Loading/LoadingOverlay';
import { GameMenu } from '../components/Menu/GameMenu';
import { screenLongSide, screenShortSide, runtimeGameVersion } from '../common/Device';
import { icon } from '../common/Icons';
import { Ui, alpha } from '../common/Theme';
import { GameProfile } from '../common/GameProfile';
import { GameButton } from '../common/Widgets';

const KEYCODE_BUTTON_SELECT: number = 2311;   // a controller's Select / View: the Switch's Minus
const SWITCH_MINUS_BIT: number = 11;

class TotkSurface extends XComponentController {
  private attached: boolean = false;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(0x4D47, 'NGGame', 'surface created %{public}s', surfaceId);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const w = Math.round(rect.surfaceWidth);
    const h = Math.round(rect.surfaceHeight);
    if (!this.attached) {
      this.attached = totk.setSurface(surfaceId, w, h) === 0;
      hilog.info(0x4D47, 'NGGame', 'surface %{public}s %{public}dx%{public}d attached=%{public}s', surfaceId, w, h,
        String(this.attached));
    } else {
      totk.surfaceChanged(w, h);
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.attached = false;
    totk.surfaceDestroyed();
  }
}

@Entry
@Component
struct Game {
  private surface: TotkSurface = new TotkSurface();
  private poll: number = -1;
  private bootPoll: number = -1;
  private controllers: number = 0;
  private tracker: BootTracker = new BootTracker();
  @State areaWidth: number = 0;
  @State areaHeight: number = 0;
  @State dualStick: boolean = false;
  @State standardPad: boolean = false;   // the Switch's own on-screen layout (setting touch_layout)
  @State showSettings: boolean = false;
  @State menuOpen: boolean = false;
  @State touchMode: string = 'auto';   // auto: hidden while a controller is connected; show; hide
  @State settings: TotkSettings = new TotkSettings();
  @State touchShown: boolean = true;
  @State textAsk: TextRequest | undefined = undefined;
  @State textValue: string = '';
  @State loading: boolean = true;
  @State bootStep: number = 0;
  @State bootOverall: number = 0;
  @State bootAmount: string = '';
  @State bootElapsed: number = 0;

  private context(): common.UIAbilityContext {
    return this.getUIContext().getHostContext() as common.UIAbilityContext;
  }

  // Settings apply at the next start: save them (env.txt) and restart the app straight back into this page.
  private applySettings(): void {
    const context = this.context();
    try {
      this.settings.save(context.filesDir, screenLongSide(), screenShortSide());
      const want: Want = { bundleName: context.abilityInfo.bundleName, abilityName: context.abilityInfo.name,
        parameters: { 'view': 'game' } };
      context.getApplicationContext().restartApp(want);
    } catch (e) {
      hilog.error(0x4D47, 'NGGame', 'apply settings: %{public}s', String(e));
    }
  }

  private goHome(): void {
    this.getUIContext().showAlertDialog({
      title: '回到启动页？',
      message: '游戏会关闭，没存档的进度会丢失。',
      primaryButton: { value: '取消', action: () => {} },
      secondaryButton: { value: '回到启动页', action: () => {
        const context = this.context();
        try {
          const want: Want = { bundleName: context.abilityInfo.bundleName, abilityName: context.abilityInfo.name };
          context.getApplicationContext().restartApp(want);
        } catch (e) {
          hilog.error(0x4D47, 'NGGame', 'back to the launcher: %{public}s', String(e));
        }
      } }
    });
  }

  private updateTouch(): void {
    this.touchShown = this.touchMode === 'show' ||
      (this.touchMode === 'auto' && (this.controllers === 0 || this.settings.get('touch_controls') === 'always'));
  }

  // Once a second: the on-screen controls hide while a controller is connected (touchMode auto).
  private checkControllers(): void {
    const status: GamepadStatus = totk.gamepadStatus();
    const n = Math.max(0, status.count);
    if (n > 0 && this.controllers === 0) {
      this.getUIContext().getPromptAction().showToast({ message: '已连接手柄' + (status.name.length > 0 ? '：' + status.name : '') });
    } else if (n === 0 && this.controllers > 0) {
      this.getUIContext().getPromptAction().showToast({ message: '手柄已断开' });
    }
    this.controllers = n;
    this.updateTouch();
  }

  // Four times a second until the first picture: the loading screen's stage and progress.
  private checkBoot(): void {
    const s: BootStatus = totk.bootStatus();
    this.tracker.update(s);
    this.bootStep = this.tracker.step;
    this.bootOverall = this.tracker.overall;
    this.bootAmount = this.tracker.amount;
    this.bootElapsed = this.tracker.elapsed;
    if (s.frames >= FRAMES_TO_SHOW) {   // the game draws black frames for a moment before its own screens
      if (this.bootPoll >= 0) clearInterval(this.bootPoll);
      this.bootPoll = -1;
      this.getUIContext().animateTo({ duration: 450, curve: Curve.EaseOut }, () => { this.loading = false; });
    }
  }

  aboutToAppear(): void {
    const context = this.context();
    this.settings = TotkSettings.load(context.filesDir);
    this.standardPad = this.settings.get('touch_layout') === 'standard';
    hilog.info(0x4D47, 'NGGame', 'start: %{public}s', totk.start(context.filesDir));
    totk.onTextInput((req: TextRequest) => {
      this.textValue = req.initial;
      this.textAsk = req;
    });
    this.poll = setInterval(() => { this.checkControllers(); }, 1000);
    this.bootPoll = setInterval(() => { this.checkBoot(); }, 250);
    this.checkBoot();
  }

  aboutToDisappear(): void {
    if (this.poll >= 0) clearInterval(this.poll);
    if (this.bootPoll >= 0) clearInterval(this.bootPoll);
    if (this.textAsk !== undefined) totk.textInputDone(this.textAsk.serial, null);
  }

  onPageShow(): void { totk.setForeground(true); }

  onPageHide(): void { totk.setForeground(false); }

  // The back gesture opens and closes the game menu; it never leaves the game by itself.
  onBackPress(): boolean {
    if (this.showSettings) {
      this.showSettings = false;
    } else {
      this.menuOpen = !this.menuOpen;
    }
    return true;
  }

  private answerText(text: string | null): void {
    if (this.textAsk === undefined) return;
    totk.textInputDone(this.textAsk.serial, text);
    this.textAsk = undefined;
  }

  @Builder
  private textDialog(req: TextRequest) {
    Column() {
      Text(req.prompt.length > 0 ? req.prompt : '输入文字').fontSize(17).fontWeight(FontWeight.Bolder).fontColor(Ui.text)
        .margin({ bottom: 12 })
      TextInput({ text: this.textValue, placeholder: req.maxLength > 0 ? '最多 ' + req.maxLength + ' 个字' : '' })
        .type(req.digitsOnly ? InputType.Number : req.secure ? InputType.Password : InputType.Normal)
        .maxLength(req.maxLength > 0 ? req.maxLength : 500)
        .defaultFocus(true)
        .backgroundColor(Ui.card2)
        .fontColor(Ui.text)
        .onChange((v: string) => { this.textValue = v; })
        .onSubmit(() => { if (this.textValue.length >= req.minLength) this.answerText(this.textValue); })
      Row() {
        GameButton({ label: '取消', onTap: () => { this.answerText(null); } }).margin({ right: 12 })
        GameButton({ label: '确定', badgeName: 'check', primary: true, active: this.textValue.length >= req.minLength,
          onTap: () => { this.answerText(this.textValue); } })
      }.margin({ top: 14 })
    }
    .width('56%')
    .padding(18)
    .backgroundColor(alpha(Ui.bg2, 0.96))
    .borderRadius({ topLeft: 22, bottomRight: 22, topRight: 8, bottomLeft: 8 })
    .border({ width: 1.5, color: alpha(GameProfile.accent, 0.5) })
    .alignItems(HorizontalAlign.End)
  }

  build() {
    Stack() {
      XComponent({ type: XComponentType.SURFACE, controller: this.surface })
        .width('100%')
        .height('100%')
        .focusable(true)
        .defaultFocus(true)
      if (this.loading) {
        LoadingOverlay({ step: this.bootStep, overall: this.bootOverall, amount: this.bootAmount, elapsed: this.bootElapsed })
          .transition(TransitionEffect.OPACITY)
      }
      if (this.areaWidth > 0 && this.areaHeight > 0) {
        if (this.touchShown && !this.loading) {
          PhoneControls({ areaWidth: this.areaWidth, areaHeight: this.areaHeight, dualStick: this.dualStick,
            standard: this.standardPad })
            .position({ x: 0, y: 0 })
        }
        // the game menu's pull tab: small and faint at the top centre, out of the keys' way; its touch area is larger
        Stack() {
          Image(icon('dpad_down')).width(11).height(11).fillColor(Ui.text)
        }
        .width(64).height(15)
        .borderRadius({ bottomLeft: 10, bottomRight: 10 })
        .backgroundColor(alpha(Ui.bg2, 0.6))
        .border({ width: { left: 1, right: 1, bottom: 1.5 }, color: alpha(GameProfile.accent, 0.7) })
        .opacity(0.55)
        .position({ x: (this.areaWidth - 64) / 2, y: 0 })
        .responseRegion({ x: -14, y: 0, width: 92, height: 34 })
        .onClick(() => { this.menuOpen = true; })
        .accessibilityText('游戏菜单')
      }
      if (this.menuOpen) {
        GameMenu({ touchMode: this.touchMode, dualStick: this.dualStick, standard: this.standardPad,
          onResume: () => { this.menuOpen = false; },
          onSettings: () => {
            this.settings = TotkSettings.load(this.context().filesDir);
            this.menuOpen = false;
            this.showSettings = true;
          },
          onTouchMode: () => {
            this.touchMode = this.touchMode === 'auto' ? 'show' : this.touchMode === 'show' ? 'hide' : 'auto';
            this.updateTouch();
          },
          onToggleLook: () => { this.dualStick = !this.dualStick; },
          onLayout: () => {
            this.standardPad = !this.standardPad;
            this.settings.set('touch_layout', this.standardPad ? 'standard' : 'custom');
            try {
              this.settings.save(this.context().filesDir, screenLongSide(), screenShortSide());   // kept for the next start
            } catch (e) {
              hilog.error(0x4D47, 'NGGame', 'save the layout: %{public}s', String(e));
            }
          },
          onHome: () => { this.goHome(); } })
      }
      if (this.showSettings) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#A6000000')
          Column() {
            SettingsPanel({ settings: $settings, gameVersion: runtimeGameVersion(), applyLabel: '应用并重启',
              onApply: () => { this.applySettings(); }, onClose: () => { this.showSettings = false; } })
          }
          .width('88%').height('92%')
          .padding(14)
          .backgroundColor(alpha(Ui.bg2, 0.97))
          .borderRadius({ topLeft: 24, bottomRight: 24, topRight: 8, bottomLeft: 8 })
          .border({ width: 1.5, color: alpha(GameProfile.accent, 0.5) })
        }.width('100%').height('100%')
      }
      if (this.textAsk !== undefined) {
        this.textDialog(this.textAsk)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    .onKeyEvent((event: KeyEvent) => {
      if (event.keyCode === KEYCODE_BUTTON_SELECT && (event.type === KeyType.Down || event.type === KeyType.Up)) {
        totk.setPadKey(SWITCH_MINUS_BIT, event.type === KeyType.Down);
        event.stopPropagation();
      }
    })
    .onAreaChange((_old: Area, area: Area) => {
      this.areaWidth = Number(area.width);
      this.areaHeight = Number(area.height);
    })
  }
}
