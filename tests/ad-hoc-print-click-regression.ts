// Regression test for clicking Print in the Sumatra-owned print preview dialog.

import { tmpPath, EXE, cmdId, runStandalone } from "./util.ts";
import { ControlCommand, withControlledSumatra } from "./control.ts";
import { writeMultiPagePdf } from "./print-util.ts";
import {
  enumWindows,
  getClassName,
  getWindowPid,
  getWindowText,
  isWindowVisible,
  postMessage,
  sleep,
  waitForWindowIdle,
  WM_COMMAND,
} from "./winapi.ts";
import { pressEscape, sendCommand, waitForFrame } from "./win-automation.ts";

const BN_CLICKED = 0;
const IDC_PP_PRINT = 1115;

function hasSafeDefaultPrinter(): boolean {
  const result = Bun.spawnSync({
    cmd: [
      "powershell.exe",
      "-NoProfile",
      "-Command",
      "(Get-CimInstance Win32_Printer | Where-Object Default | Select-Object -ExpandProperty Name)",
    ],
    stdout: "pipe",
    stderr: "ignore",
  });
  return result.exitCode === 0 && new TextDecoder().decode(result.stdout).trim() === "Microsoft Print to PDF";
}

function findDialog(pid: number, frame: number, title?: string): number {
  let result = 0;
  enumWindows((hwnd) => {
    if (hwnd !== frame && getWindowPid(hwnd) === pid && getClassName(hwnd) === "#32770" && isWindowVisible(hwnd)) {
      if (!title || getWindowText(hwnd) === title) {
        result = hwnd;
        return false;
      }
    }
    return true;
  });
  return result;
}

async function waitForDialog(pid: number, frame: number, title?: string): Promise<number> {
  const deadline = Date.now() + 5000;
  for (;;) {
    const dialog = findDialog(pid, frame, title);
    if (dialog) {
      return dialog;
    }
    if (Date.now() >= deadline) {
      throw new Error(`print-click: dialog${title ? ` '${title}'` : ""} did not open`);
    }
    await sleep(50);
  }
}

export async function testit(): Promise<void> {
  if (!hasSafeDefaultPrinter()) {
    console.log("⚠ print-click: skipped because Microsoft Print to PDF is not the default printer");
    return;
  }

  const pdf = tmpPath("print-click-regression.pdf");
  writeMultiPagePdf(pdf, ["one"]);

  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      await client.waitForRenderIdle(30000);
      sendCommand(frame, cmdId("CmdPrint"));

      const dialog = await waitForDialog(proc.pid!, frame, "Print");
      if (!(await waitForWindowIdle(dialog, 5000))) {
        throw new Error("print-click: preview did not settle");
      }
      if (!postMessage(dialog, WM_COMMAND, IDC_PP_PRINT | (BN_CLICKED << 16), 0)) {
        throw new Error("print-click: could not click Print");
      }

      await sleep(300);
      await client.request(ControlCommand.Ping);

      const outputDialog = await waitForDialog(proc.pid!, frame);
      await pressEscape(outputDialog);
    },
    [pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
