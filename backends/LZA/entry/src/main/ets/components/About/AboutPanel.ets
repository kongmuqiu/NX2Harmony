// The launcher's 关于 page: this app (bundle name, version), the game version the runtime runs, where the game data
// and the runtime's log are, and the licences of the open source parts inside (resources/rawfile/licenses).
import { common, bundleManager } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { badge } from '../../common/Icons';
import { Ui, alpha } from '../../common/Theme';
import { GameProfile } from '../../common/GameProfile';
import { SectionTitle } from '../../common/Widgets';
import { runtimeGameVersion } from '../../common/Device';

class InfoRow {
  label: string;
  value: string;
  constructor(label: string, value: string) { this.label = label; this.value = value; }
}

@Component
export struct AboutPanel {
  @State rows: InfoRow[] = [];
  @State licenses: string[] = [];

  aboutToAppear(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    let version = '';
    try {
      version = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT).versionName;
    } catch (e) {
      version = '';
    }
    this.rows = [
      new InfoRow('游戏', GameProfile.title + ' ' + runtimeGameVersion()),
      new InfoRow('应用版本', version),
      new InfoRow('包名', context.abilityInfo.bundleName),
      new InfoRow('游戏数据', 'files/' + GameProfile.dataDir),
      new InfoRow('运行日志', 'files/android/totk-native.log'),
    ];
    try {
      this.licenses = context.resourceManager.getRawFileListSync('licenses').sort();
    } catch (e) {
      this.licenses = [];
    }
  }

  private showLicense(name: string): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    let text = '';
    try {
      const bytes = context.resourceManager.getRawFileContentSync('licenses/' + name);
      text = util.TextDecoder.create('utf-8').decodeToString(bytes);
    } catch (e) {
      text = String(e);
    }
    this.getUIContext().showAlertDialog({ title: name, message: text, confirm: { value: '关闭', action: () => {} } });
  }

  build() {
    Scroll() {
      Column() {
        SectionTitle({ badgeName: 'info', title: '关于' })
        ForEach(this.rows, (r: InfoRow) => {
          Row() {
            Text(r.label).fontSize(13).fontColor(Ui.text3).width(72)
            Text(r.value).fontSize(13).fontWeight(FontWeight.Bold).fontColor(Ui.text).layoutWeight(1)
              .textAlign(TextAlign.End).maxLines(2)
          }.width('100%').padding({ top: 8, bottom: 8 }).border({ width: { bottom: 1 }, color: Ui.line })
        }, (r: InfoRow) => r.label)
        Text('游戏数据、着色器和改写后的游戏代码都来自你自己的游戏文件，只在这台手机上，请勿分享。')
          .fontSize(11).fontColor(Ui.text3).margin({ top: 8, bottom: 14 })
        SectionTitle({ badgeName: 'layers', title: '开源许可', note: this.licenses.length + ' 项' })
        ForEach(this.licenses, (n: string) => {
          Row() {
            Text(n.replace('.txt', '')).fontSize(13).fontColor(Ui.text2).layoutWeight(1)
            Text('查看').fontSize(12).fontWeight(FontWeight.Bold).fontColor(GameProfile.accent)
          }
          .width('100%')
          .padding({ left: 12, right: 12, top: 10, bottom: 10 })
          .margin({ bottom: 6 })
          .borderRadius({ topLeft: 12, bottomRight: 12, topRight: 4, bottomLeft: 4 })
          .backgroundColor(alpha(Ui.card2, 0.85))
          .onClick(() => { this.showLicense(n); })
        }, (n: string) => n)
      }.width('100%').alignItems(HorizontalAlign.Start)
    }.width('100%').height('100%').scrollBar(BarState.Off).align(Alignment.Top).edgeEffect(EdgeEffect.Spring)
  }
}
