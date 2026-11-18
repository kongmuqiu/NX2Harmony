// The game page's loading screen until the game shows its picture, full screen: the game's art, two rings turning
// around its icon, and one progress bar for the whole start. The runtime reports its boot stages (libtotk_ohos.so
// bootStatus(), frontend_ohos.c); BootTracker turns them into six steps, each with its own share of the bar:
//   1 准备运行环境   starting the runtime, reading the data index
//   2 校验游戏数据   the first start only: the RomFS image's SHA-256, by bytes
//   3 挂载游戏数据   the verification record, the resolution patch and the read-only mount
//   4 初始化游戏模块 the modules' initializers, (k/n)
//   5 游戏启动中     the game's own start, which reports nothing until it draws: the bar creeps on by time, never full
//   6 进入画面       the first frames, until the page lifts the screen
// The bar only grows. A tip changes every few seconds.
import { BootStatus } from 'libtotk_ohos.so';
import { GameProfile, TIPS } from '../../common/GameProfile';
import { Ui, alpha, formatBytes } from '../../common/Theme';

export const STEPS: string[] = ['准备运行环境', '校验游戏数据', '挂载游戏数据', '初始化游戏模块', '游戏启动中', '进入画面'];
export const FRAMES_TO_SHOW: number = 45;   // the game draws black frames for a moment before its own screens

export class BootTracker {
  step: number = 0;
  inner: number = 0;          // within the step, 0..1
  overall: number = 0;        // the whole start, 0..1, never going back
  amount: string = '';        // e.g. '8.9 GB / 17.1 GB'
  elapsed: number = 0;
  private gameStart: number = -1;   // elapsed seconds when step 5 began

  update(s: BootStatus): void {
    const stage = s.stage.toLowerCase();
    let step = 0;
    let inner = 0;
    let amount = '';
    const m = s.detail.match(/\((\d+)\/(\d+)\)/);
    if (s.frames > 0) {
      step = 5;
      inner = Math.min(1, s.frames / FRAMES_TO_SHOW);
    } else if (stage.indexOf('verifying game data') >= 0) {
      step = 1;
      inner = s.total > 0 ? s.done / s.total : 0;
      amount = s.total > 0 ? formatBytes(s.done) + ' / ' + formatBytes(s.total) : '';
    } else if (stage.indexOf('verification record') >= 0 || stage.indexOf('preparing game data') >= 0 ||
      stage.indexOf('mounting') >= 0) {
      step = 2;
      inner = 0.5;
    } else if (stage.indexOf('initializing game modules') >= 0 && m !== null && Number(m[1]) < Number(m[2])) {
      step = 3;
      inner = (Number(m[1]) - 1) / Number(m[2]);
      amount = m[1] + ' / ' + m[2];
    } else if (stage.indexOf('initializing game modules') >= 0 || stage.indexOf('shader') >= 0) {
      step = 4;   // the last initializer runs into the game's own start
      if (this.gameStart < 0) this.gameStart = s.elapsed;
      inner = 1 - Math.exp(-(s.elapsed - this.gameStart) / 6);
      if (stage.indexOf('shader') >= 0 && s.total > 0) {
        inner = s.done / s.total;
        amount = s.done + ' / ' + s.total;
      }
    }
    this.step = step;
    this.inner = Math.max(0, Math.min(1, inner));
    this.overall = Math.max(this.overall, (step + this.inner) / STEPS.length);
    this.amount = amount;
    this.elapsed = s.elapsed;
  }
}

@Component
export struct LoadingOverlay {
  @Prop step: number = 0;
  @Prop overall: number = 0;
  @Prop amount: string = '';
  @Prop elapsed: number = 0;
  @State private angle: number = 0;
  @State private shine: number = 0;
  @State private tip: number = 0;
  private timer: number = -1;

  aboutToAppear(): void {
    const ui = this.getUIContext();
    ui.animateTo({ duration: 1400, iterations: -1, curve: Curve.Linear }, () => { this.angle = 360; });
    ui.animateTo({ duration: 1600, iterations: -1, curve: Curve.EaseInOut }, () => { this.shine = 1; });
    this.tip = Math.floor(Math.random() * TIPS.length);
    this.timer = setInterval(() => { this.tip = (this.tip + 1) % TIPS.length; }, 5000);
  }

  aboutToDisappear(): void {
    if (this.timer >= 0) clearInterval(this.timer);
  }

  @Builder
  private stepDots() {
    Row({ space: 6 }) {
      ForEach(STEPS, (_s: string, i: number) => {
        Column()
          .width(i === this.step ? 22 : 8)
          .height(8)
          .borderRadius(4)
          .backgroundColor(i < this.step ? GameProfile.accent : i === this.step ? GameProfile.accent2 : '#2A3446')
          .animation({ duration: 250, curve: Curve.Friction })
      }, (s: string) => s)
    }
  }

  build() {
    Stack() {
      Image($r('app.media.game_icon')).width('100%').height('100%').objectFit(ImageFit.Cover).blur(70).opacity(0.4)
      Column().width('100%').height('100%')
        .radialGradient({ center: ['50%', '42%'], radius: '80%', colors: [[alpha(Ui.bg, 0.25), 0], [alpha(Ui.bg, 0.9), 0.7], [Ui.bg, 1]] })
      Column() {
        Stack() {
          Circle({ width: 152, height: 152 }).fill(Color.Transparent).stroke(alpha(GameProfile.accent, 0.14)).strokeWidth(6)
          Circle({ width: 152, height: 152 }).fill(Color.Transparent).stroke(GameProfile.accent).strokeWidth(6)
            .strokeDashArray([100, 380]).strokeLineCap(LineCapStyle.Round).rotate({ angle: this.angle })
          Circle({ width: 128, height: 128 }).fill(Color.Transparent).stroke(GameProfile.accent2).strokeWidth(3)
            .strokeDashArray([44, 360]).strokeLineCap(LineCapStyle.Round).rotate({ angle: -this.angle * 1.5 })
          Image($r('app.media.game_icon')).width(96).height(96).borderRadius(26)
            .shadow({ radius: 30, color: alpha(GameProfile.accent, 0.55), offsetY: 4 })
        }.width(158).height(158)
        Text(GameProfile.title).fontSize(22).fontWeight(FontWeight.Bolder).fontColor(Ui.text).margin({ top: 14 })
        Row() {
          Text('第 ' + (this.step + 1) + ' / ' + STEPS.length + ' 步').fontSize(11).fontWeight(FontWeight.Bolder)
            .fontColor('#0B1118').backgroundColor(GameProfile.accent).borderRadius(6)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 }).margin({ right: 8 })
          Text(STEPS[Math.min(this.step, STEPS.length - 1)]).fontSize(15).fontWeight(FontWeight.Bold).fontColor(GameProfile.accent)
        }.margin({ top: 8 })
        Stack({ alignContent: Alignment.Start }) {
          Column().width('100%').height(12).borderRadius(6).backgroundColor('#1E2738')
          Column().width((this.overall * 100).toFixed(1) + '%').height(12).borderRadius(6)
            .linearGradient({ angle: 90, colors: [[GameProfile.accent, 0], [GameProfile.accent2, 1]] })
            .shadow({ radius: 10, color: alpha(GameProfile.accent, 0.6) })
            .animation({ duration: 300, curve: Curve.Linear })
          Column().width(60).height(12)
            .linearGradient({ angle: 90, colors: [['#00FFFFFF', 0], ['#66FFFFFF', 0.5], ['#00FFFFFF', 1]] })
            .translate({ x: this.shine * 420 - 60 })
        }.width(420).height(12).margin({ top: 14 }).borderRadius(6).clip(true)
        Row() {
          this.stepDots()
          Blank()
          Text((this.amount.length > 0 ? this.amount + ' · ' : '') + Math.floor(this.overall * 100) + '%')
            .fontSize(12).fontWeight(FontWeight.Bold).fontColor(Ui.text2)
        }.width(420).margin({ top: 10 })
        Row() {
          Text('已用 ' + Math.floor(this.elapsed) + ' 秒').fontSize(11).fontColor(Ui.text3)
        }.width(420).justifyContent(FlexAlign.End).margin({ top: 4 })
        if (this.step === 1) {
          Text(GameProfile.firstBoot).fontSize(12).fontColor(Ui.warn).margin({ top: 8 }).width(420).textAlign(TextAlign.Center)
        }
        Row() {
          Text('小提示').fontSize(11).fontWeight(FontWeight.Bolder).fontColor('#0B1118').backgroundColor(GameProfile.accent2)
            .borderRadius(6).padding({ left: 6, right: 6, top: 2, bottom: 2 }).margin({ right: 8 })
          Text(TIPS[this.tip]).fontSize(12).fontColor(Ui.text2).layoutWeight(1).maxLines(2)
        }.width(500).margin({ top: 18 })
      }.alignItems(HorizontalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Ui.bg)
  }
}
