"""One-click HarmonyOS builds of the Switch games this project has adapted (games.tsv), from the player's own files.

    nx2h setup [--fix]                  check this PC and the phone; --fix installs what winget can
    nx2h scan <folder>...               which games and versions the folders hold, and what each still needs
    nx2h build [<folder>...] [-k <folder with prod.keys>] [--game <title id>] [--no-install] [--progress json]
               [--screen <width>x<height>]
                                        prepare, build the signed HAP, install it and send the game data; the game's
                                        UI is laid out for the phone's screen (asked from the phone, or --screen)

(`python nx2h.py ...` from a checkout, `nx2h.exe ...` next to the packed NX2Harmony.exe.) Exit codes as nxnative's
build_ipa_from_nsp.py: 0 done, 1 stopped on an error (the last [RED] line says why), 2 bad arguments, 130 interrupted.
Nothing the build makes from game files, keys or signing ever goes into the repository."""
from __future__ import annotations

import argparse
import contextlib
import sys
import time
from pathlib import Path

from . import agc, config, doctor, scan, signing
from .device import Hdc, parse_size
from .events import Events
from .pick import pick_folder

from .backends import load as backend_class, missing as backend_missing


def describe(items: list) -> list:
    """[(games.tsv row or None, base title, items)] for the games found."""
    rows = {r["title_id"]: r for r in config.games()}
    return [(rows.get(t), t, its) for t, its in scan.games_found(items).items()]


def screen_arg(text: str) -> tuple:
    """--screen 2848x1320 (either way round) as a landscape (width, height)."""
    size = parse_size(text)
    if not size:
        raise argparse.ArgumentTypeError(f"要写成 宽x高（像素），比如 2848x1320：{text}")
    return size


def cmd_scan(a) -> int:
    items = scan.find(a.folders)
    if not items:
        print("没有找到游戏文件（.nsp .nsz .xci .xcz .nup）")
        return 1
    for row, title, its in describe(items):
        from .core import file_title
        name = row["name"] if row else file_title(its)
        print(f"\n{name}  [{title}]")
        for it in its:
            vers = ", ".join(f"{r} {v or '?'}" for _, v, r in it.titles)
            print(f"  {it.kind.upper():4} {vers:28} {it.path}" + (f"  ({it.note})" if it.note else ""))
        if not row:
            print("  状态：未适配。一键构建只支持 games.tsv 里已验证或优化中的版本；新游戏要先按 docs/DESIGN.md 的流程适配")
        elif row["status"] not in config.BUILDABLE or row["backend"] == "-":
            print(f"  状态：{row['status']}（{row['version']}）。还不能一键构建")
        else:
            gone = backend_missing(row["backend"])
            be = None if gone else backend_class(row["backend"])(Events(), Hdc())
            # as the library page (core.library_games): a game whose data is prepared needs no game files any more
            problems = [gone] if gone else ([] if not be.needs_inputs() else be.pick_inputs(its)[1])
            print(f"  状态：{'已适配' if row['status'] == '已验证' else row['status']} {row['version']}。"
                  + ("文件齐全，可以一键构建" if not problems else "；".join(problems)))
    for it in items:
        if not it.titles and it.note:
            print(f"\n无法识别：{it.path}（{it.note}）")
    return 0


def cmd_setup(a) -> int:
    ev = Events(a.progress == "json")
    cs = doctor.checks()
    if a.fix and any(c.ok is False and c.fix for c in cs):
        cs = doctor.fix(cs)
    doctor.show(cs, ev)
    bad = [c for c in cs if c.ok is False]
    if not ev.json:
        print("\n环境就绪，可以构建。" if not bad else f"\n还有 {len(bad)} 项没准备好" + ("（可自动处理的用 setup --fix）" if any(c.fix for c in bad) else ""))
    return 0 if not bad else 1


def cmd_build(a) -> int:
    ev = Events(a.progress == "json")
    if not a.folders:   # 一键构建.cmd: ask, starting from the folder picked last time
        d = pick_folder("选择放游戏文件的文件夹（里面的子文件夹都会扫描，可以放多个游戏）", config.local().get("library", ""))
        if not d:
            return 2
        config.save_local(library=d)
        a.folders = [d]
    items = scan.find(a.folders)
    found = [(row, t, its) for row, t, its in describe(items) if row and row["status"] in config.BUILDABLE and row["backend"] != "-"]
    if a.game:
        found = [f for f in found if f[1] == a.game.upper()]
    if not found:
        ev.red("这些文件夹里没有已适配、可一键构建的游戏（python nx2h.py scan 看详情）")
        return 1
    if len(found) > 1:
        ev.red("找到不止一个可构建的游戏，用 --game <title id> 选一个：" + ", ".join(f"{r['name']} [{t}]" for r, t, _ in found))
        return 1
    row, title, its = found[0]
    gone = backend_missing(row["backend"])
    if gone:
        ev.red(gone)
        return 1
    hdc = Hdc()
    be = backend_class(row["backend"])(ev, hdc)
    be.screen = a.screen   # totk-ohos: the screen the UI is laid out for; None: the backend asks the phone
    inputs, problems = be.pick_inputs(its)
    keys = Path(a.keys) if a.keys else (config.key_candidates() or [None])[0]
    if keys and keys.is_file():
        keys = keys.parent if keys.name == "prod.keys" else None
    tag = a.tag or "nx2h-" + time.strftime("%Y%m%d-%H%M")
    ev.game(title_id=title, name=row["name"], version=row["version"], bundle_id=be.bundle, out=str(be.hap_path(tag)), icon=None)

    need_inputs, need_keys = be.needs_inputs(), be.needs_keys()
    if need_inputs and problems:
        ev.red("；".join(problems))
        return 1
    if need_inputs:   # before the first step: the unpack alone writes tens of GB
        short = doctor.disk_short(be.root, getattr(be, "disk_gb", 80), be.name)
        if short:
            ev.red(short)
            return 1
    if need_keys and not (keys and (keys / "prod.keys").is_file()) and not ev.json:
        d = pick_folder("选择 prod.keys 所在的文件夹（你自己的 Switch 导出的）")
        if d and (Path(d) / "prod.keys").is_file():
            keys = Path(d)
            config.save_local(keys=d)   # the folder only; the file is read by the backend's containers, never copied
    if need_keys and not (keys and (keys / "prod.keys").is_file()):
        ev.red("需要 prod.keys：用 -k 指定它所在的文件夹（或放到 %USERPROFILE%\\.switch）")
        return 1
    install = not a.no_install
    if install and not hdc.pick():
        ev.red("没有找到连接的手机（setup 里有开启开发者模式和 USB 调试的步骤）；只想出包可以加 --no-install")
        return 1
    if not config.python():
        ev.red("构建要用本机的 Python 3.9 或更新版本来运行后端脚本：在“环境”页一键安装（或 winget install -e --id Python.Python.3.12 --source winget），装好后重新打开软件")
        return 1
    # the phone's screen stays on from here (signing, the build, the install, the push) until the game is open, then
    # gets its own screen-off time back, also after a failure (keep_awake); the app hears it (ev.awake) to put it back
    # itself when it kills the build
    with contextlib.ExitStack() as awake:
        if install:
            awake.enter_context(hdc.keep_awake(report=lambda on, restore: ev.awake(on, hdc.target, hdc.server, restore)))
            ev.say("构建、安装和推送期间手机保持亮屏，结束后恢复原来的息屏时间")
        return _build(ev, row, title, its, items, be, hdc, inputs, keys, tag, install)


def _build(ev, row, title, its, items, be, hdc, inputs, keys, tag, install) -> int:
    """cmd_build's signing and stages, once the game, its inputs, the keys and the phone are known."""
    udid = agc.phone_udid(hdc) if install else ""
    if not signing.signable(be.bundle) or (udid and not signing.covers(be.bundle, udid)):
        # signed in the background (agc.py): a Huawei account login in the browser the first time, then nothing to do
        ev.say(f"签名：{be.bundle} " + ("还没签名" if not signing.signable(be.bundle) else "的签名里没有这台手机")
               + "，在后台向华为申请（第一次会在浏览器里打开华为账号登录页）")
        try:
            agc.ensure(be.bundle, say=ev.say, hdc=hdc if install else None)
        except agc.SigningError as e:
            if not signing.deveco_exe().is_file():   # no DevEco Studio to sign by hand (a toolchain without it)
                ev.red(f"后台签名没成功：{e}" + ("，装好后重新构建。" if isinstance(e, agc.NoJava) else "。检查华为账号后重新构建。"))
                return 1
            # fallback: a small project for this name, opened in DevEco Studio to sign once by hand
            proj = signing.project_for(be.bundle, be.bundle.rsplit(".", 1)[-1], be.root)
            opened = signing.open_in_deveco(proj)
            ev.signing(be.bundle, str(proj), opened)
            ev.red(f"后台签名没成功：{e}。也可以用 DevEco Studio 签：签名工程 {proj}"
                   + ("（正在打开）" if opened else "") + "。" + signing.HOWTO)
            return 1

    extra = be.extra_stages() if install else []
    ev.plan((5 + len(extra)) if install else 3)
    ev.stage("准备游戏数据", "解包、改写、着色器")
    if not be.prepare(inputs, keys, items):
        ev.stage_end(False, "准备失败")
        return 1
    ev.stage_end(True, "游戏数据就绪")
    ev.stage("编译原生库")
    if not be.libs():
        ev.stage_end(False, "编译失败")
        return 1
    ev.stage_end(True, "libtotk_ohos.so / libtotk_guest.so")
    ev.stage("打包 HAP", "签名并审计")
    from . import icon as icons
    icons.extract(title, its, str(keys) if keys else None)   # the game's own icon, else its eShop icon (once; kept)
    game_icon = icons.cached(title)
    if not game_icon:
        ev.say("    这个游戏没取到图标（文件里没有，网上也没查到）：HAP 用默认图标")
    hap = be.hap(tag, icon=game_icon)
    if not hap:
        ev.stage_end(False, "打包失败")
        return 1
    ev.stage_end(True, f"{hap.name}（{hap.stat().st_size / 2**20:.0f} MB）")
    if not install:
        ev.say(f"完成：{hap}。装到手机：hdc install -r <HAP>，再运行 python nx2h.py build ... 推送数据")
        return 0
    ev.stage("安装到手机", hdc.target)
    if not be.install(hap):
        ev.stage_end(False, "安装失败")
        return 1
    mark_game(ev, hdc, be.bundle)
    ev.stage_end(True, "已安装")
    ev.stage("推送游戏数据", "手机上已有的同一份会跳过")
    if not be.push():
        ev.stage_end(False, "推送失败")
        return 1
    ev.stage_end(True, "数据就绪")
    for name, detail, run in extra:
        ev.stage(name, detail)
        if not run():
            ev.stage_end(False, f"{name}没完成")
            return 1
        ev.stage_end(True, "完成")
    be.launch()
    ev.say(f"完成：{row['name']} {row['version']} 已装到手机并打开。")
    return 0


def mark_game(ev, hdc, bundle: str) -> None:
    """The installed app marked as a game for the system (its game scheduling); one line either way, never a failure."""
    ok, said = hdc.mark_game(bundle)
    if ok:
        ev.say("    已标记为游戏：系统按游戏策略调度")
    else:
        why = next((l.strip() for l in (said or "").splitlines() if l.strip()), "手机没有回应")
        ev.say(f"    没能标记为游戏（不影响游玩）：{why[:100]}")


def utf8_output() -> None:
    """Chinese text on any Windows console (often code page 936) and UTF-8 down a pipe (the app reads it so)."""
    if sys.platform == "win32" and sys.stdout is not None and sys.stdout.isatty():
        import ctypes
        ctypes.windll.kernel32.SetConsoleOutputCP(65001)
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    utf8_output()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup", help="检查环境（--fix 自动安装能装的）")
    s.add_argument("--fix", action="store_true")
    s.add_argument("--progress", choices=("text", "json"), default="text")
    s = sub.add_parser("scan", help="识别文件夹里的游戏")
    s.add_argument("folders", nargs="+")
    s = sub.add_parser("build", help="一键构建并装到手机")
    s.add_argument("folders", nargs="*", help="放游戏文件的文件夹（不给则弹窗选择）")
    s.add_argument("-k", "--keys", help="prod.keys 所在的文件夹（或文件本身）")
    s.add_argument("--game", help="文件夹里有几个游戏时，选哪个（title id）")
    s.add_argument("--tag", help="HAP 的标签（默认 nx2h-<时间>）")
    s.add_argument("--no-install", action="store_true", help="只出 HAP，不装到手机")
    s.add_argument("--screen", type=screen_arg, metavar="宽x高",
                   help="手机屏幕的分辨率，比如 2848x1320：按它生成游戏的界面布局（HUD 贴边）；不给就从连着的手机读")
    s.add_argument("--progress", choices=("text", "json"), default="text")
    a = ap.parse_args()
    try:
        return {"setup": cmd_setup, "scan": cmd_scan, "build": cmd_build}[a.cmd](a)
    except KeyboardInterrupt:
        return 130
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            return 1
        raise


