// The settings page (launcher and in-game): preset cards (the one all current values match is lit, else 自定义), the
// groups (a rail on the left when wide, chips on top when narrow) and each option of the group as a card of choices.
// Options for another game version than the runtime's (gameVersion, libtotk_ohos.so gameVersion()) are not shown, nor
// 界面位置 unless 画面比例 is 全面屏 (SettingsModel optionShown): it comes and goes as soon as 画面比例 is picked.
// It edits a TotkSettings: onChanged after every change (the launcher saves at once); with an applyLabel it shows the
// footer whose button calls onApply (in game: save and restart).
import { TotkSettings, OPTIONS, GROUPS, PRESETS, SettingOption, Choice, Preset, optionShown } from './SettingsModel';
import { badge } from '../../common/Icons';
import { Ui, alpha } from '../../common/Theme';
import { GameProfile } from '../../common/GameProfile';
import { GameButton } from '../../common/Widgets';

function groupBadge(g: string): string {
  switch (g) {
    case '性能': return 'gauge';
    case '渲染': return 'layers';
    case '画质': return 'sparkle';
    case '画面': return 'display';
    case '操作': return 'controller';
    default: return 'settings';
  }
}

function presetBadge(name: string): string {
  switch (name) {
    case '流畅': return 'gauge';
    case '画质': return 'sparkle';
    case '高帧率': return 'play';
    default: return 'settings';
  }
}

@Component
export struct SettingsPanel {
  @Link settings: TotkSettings;
  @State version: number = 0;   // bumped on every change: the cards re-read the map
  @State group: string = GROUPS[0];
  @State wide: boolean = false;
  gameVersion: string = '1.4.3';
  applyLabel: string = '';
  onApply: () => void = () => {};
  onClose?: () => void;
  onChanged: () => void = () => {};

  private pick(key: string, value: string): void {
    this.settings.set(key, value);
    this.version++;
    this.onChanged();
  }

  // reads version (bumped by every pick) so the cards' ForEach runs again when a value another card depends on changes
  private shown(g: string): SettingOption[] {
    return OPTIONS.filter((o: SettingOption) => o.group === g && this.version >= 0 &&
      optionShown(o, this.settings, this.gameVersion));
  }

  private groups(): string[] {
    return GROUPS.filter((g: string) => this.shown(g).length > 0);
  }

  private current(): string {
    return this.version >= 0 ? this.settings.matchingPreset(this.gameVersion) : '';
  }

  @Builder
  private presetCard(p: Preset) {
    Column() {
      Row() {
        Image(badge(presetBadge(p.name))).width(24).height(24).margin({ right: 6 })
        Text(p.name).fontSize(15).fontWeight(FontWeight.Bolder).fontColor(Ui.text)
        Blank()
        if (this.current() === p.name) {
          Text('当前').fontSize(10).fontWeight(FontWeight.Bold).fontColor('#0B1118').backgroundColor(GameProfile.accent)
            .borderRadius(6).padding({ left: 6, right: 6, top: 1, bottom: 1 })
        }
      }.width('100%')
      Text(p.caption).fontSize(10).fontColor(Ui.text3).maxLines(this.wide ? 1 : 2).margin({ top: 4 })
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width(172)
    .alignItems(HorizontalAlign.Start)
    .padding({ left: 12, right: 12, top: 9, bottom: 9 })
    .margin({ right: 8 })
    .borderRadius({ topLeft: 16, bottomRight: 16, topRight: 6, bottomLeft: 6 })
    .backgroundColor(this.current() === p.name ? alpha(GameProfile.accent, 0.16) : Ui.card2)
    .border({ width: this.current() === p.name ? 1.5 : 1, color: this.current() === p.name ? GameProfile.accent : Ui.line })
    .onClick(() => { this.settings.applyPreset(p); this.version++; this.onChanged(); })
  }

  @Builder
  private groupButton(g: string, vertical: boolean) {
    Row() {
      Image(badge(groupBadge(g))).width(24).height(24).margin({ right: 8 }).opacity(this.group === g ? 1 : 0.6)
      Text(g).fontSize(14).fontWeight(this.group === g ? FontWeight.Bolder : FontWeight.Medium)
        .fontColor(this.group === g ? Ui.text : Ui.text2)
    }
    .width(vertical ? '100%' : undefined)
    .padding({ left: 10, right: 14, top: 7, bottom: 7 })
    .margin(vertical ? { bottom: 6 } : { right: 6 })
    .borderRadius(14)
    .backgroundColor(this.group === g ? alpha(GameProfile.accent, 0.2) : Color.Transparent)
    .border({ width: this.group === g ? 1 : 0, color: alpha(GameProfile.accent, 0.8) })
    .onClick(() => { this.group = g; })
  }

  @Builder
  private choicePill(o: SettingOption, c: Choice) {
    Text(c.label)
      .fontSize(13)
      .fontWeight(this.version >= 0 && this.settings.get(o.key) === c.value ? FontWeight.Bolder : FontWeight.Medium)
      .fontColor(this.version >= 0 && this.settings.get(o.key) === c.value ? '#0B1118' : Ui.text2)
      .backgroundColor(this.version >= 0 && this.settings.get(o.key) === c.value ? GameProfile.accent : '#222C3E')
      .borderRadius({ topLeft: 10, bottomRight: 10, topRight: 4, bottomLeft: 4 })
      .padding({ left: 13, right: 13, top: 7, bottom: 7 })
      .margin({ right: 8, bottom: 8 })
      .onClick(() => this.pick(o.key, c.value))
  }

  @Builder
  private optionCard(o: SettingOption) {
    Column() {
      Text(o.title).fontSize(15).fontWeight(FontWeight.Bolder).fontColor(Ui.text).margin({ bottom: 8 })
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach(o.choices, (c: Choice) => { this.choicePill(o, c) }, (c: Choice) => o.key + c.value)
      }
      if (o.note.length > 0) {
        Text(o.note).fontSize(11).fontColor(Ui.text3)
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .margin({ bottom: 8 })
    .borderRadius({ topLeft: 14, bottomRight: 14, topRight: 6, bottomLeft: 6 })
    .backgroundColor(alpha(Ui.card2, 0.85))
    .border({ width: 1, color: Ui.line })
  }

  @Builder
  private options() {
    Scroll() {
      Column() {
        ForEach(this.shown(this.group), (o: SettingOption) => { this.optionCard(o) }, (o: SettingOption) => o.key)
      }.width('100%').alignItems(HorizontalAlign.Start)
    }.layoutWeight(1).scrollBar(BarState.Off).align(Alignment.Top).edgeEffect(EdgeEffect.Spring)
  }

  build() {
    Column() {
      Row() {
        Scroll() {
          Row() {
            ForEach(PRESETS, (p: Preset) => { this.presetCard(p) }, (p: Preset) => p.name)
          }
        }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).layoutWeight(1)
        if (this.current() === '') {
          Text('自定义').fontSize(12).fontWeight(FontWeight.Bold).fontColor(Ui.warn).margin({ left: 8 })
        }
      }.width('100%').margin({ bottom: 10 })
      if (this.wide) {
        Row() {
          Scroll() {
            Column() {
              ForEach(this.groups(), (g: string) => { this.groupButton(g, true) }, (g: string) => g)
            }
          }.width(128).height('100%').margin({ right: 10 }).scrollBar(BarState.Off).align(Alignment.Top)
          this.options()
        }.width('100%').layoutWeight(1).alignItems(VerticalAlign.Top)
      } else {
        Scroll() {
          Row() {
            ForEach(this.groups(), (g: string) => { this.groupButton(g, false) }, (g: string) => g)
          }
        }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).width('100%').margin({ bottom: 8 })
        this.options()
      }
      if (this.applyLabel.length > 0) {
        Row() {
          Text('设置在下次启动游戏时生效').fontSize(11).fontColor(Ui.text3)
          Blank()
          if (this.onClose) {
            GameButton({ label: '取消', onTap: () => { if (this.onClose) this.onClose(); } }).margin({ right: 10 })
          }
          GameButton({ label: this.applyLabel, badgeName: 'check', primary: true, onTap: () => { this.onApply(); } })
        }.width('100%').margin({ top: 8 })
      }
    }
    .width('100%').height('100%')
    .onAreaChange((_o: Area, a: Area) => { this.wide = Number(a.width) > 560; })
  }
}
