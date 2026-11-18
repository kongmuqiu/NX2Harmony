import { UIAbility, Want, AbilityConstant } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import totk from 'libtotk_ohos.so';
import { StartCrashDiagnostics, CollectCrashDiagnostics } from '../diagnostics/CrashDiagnostics';

// The app opens on the launcher (settings + start). Two launch parameters are for the PC-side tools:
//   --ps view game      open the game page directly
//   --ps command faults collect crash diagnostics into files/
export default class EntryAbility extends UIAbility {
  private initialPage: string = 'pages/Launcher';

  private dispatchCommand(want: Want): void {
    const command = want.parameters?.command;
    if (command === 'faults') {
      this.captureFaults(true);
      return;
    }
    if (command === 'start') {
      // Starts the runtime without the game page (log: files/android/totk-native.log).
      hilog.info(0x4D47, 'NGGame', 'start: %{public}s', totk.start(this.context.filesDir));
    }
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    if (want.parameters?.view === 'game') this.initialPage = 'pages/Game';
    this.captureFaults(false);
    this.dispatchCommand(want);
  }

  private async captureFaults(explicit: boolean): Promise<void> {
    try {
      const result = explicit ? await CollectCrashDiagnostics(this.context.filesDir) :
        await StartCrashDiagnostics(this.context.filesDir);
      hilog.info(0x4D47, 'NGFaults', '%{public}s', result);
    } catch (error) {
      hilog.error(0x4D47, 'NGFaults', 'capture failed: %{public}s', String(error));
    }
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.dispatchCommand(want);
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    this.configureWindow(stage);
  }

  // Landscape and immersive for every page (the launcher too, as a game launcher): full-screen layout, no status bar,
  // no navigation indicator; a swipe from the edge still brings them back for a moment.
  private async configureWindow(stage: window.WindowStage): Promise<void> {
    try {
      const main = stage.getMainWindowSync();
      await main.setPreferredOrientation(window.Orientation.LANDSCAPE);
      await main.setWindowLayoutFullScreen(true);
      await main.setSpecificSystemBarEnabled('status', false);
      await main.setSpecificSystemBarEnabled('navigationIndicator', false);
    } catch (error) {
      hilog.error(0x4D47, 'NGGame', 'window configuration: %{public}s', String(error));
    }
    this.loadInitialPage(stage);
  }

  private loadInitialPage(stage: window.WindowStage): void {
    stage.loadContent(this.initialPage, (error) => {
      if (error.code) {
        hilog.error(0x4D47, 'NGGame', 'loadContent: %{public}s', JSON.stringify(error));
      }
    });
  }
}
