/**
 * 诊断脚本(只给 CI 用):打开一份文档,若 helper 卡住,就用 Windows 调试器 cdb 抓全部线程栈。
 *
 * 需求:Windows 上 LibreOfficeKit 加载 Calc / Impress / Draw 文档时 documentLoad 永不返回
 * (导入进度已到 100%;Writer 正常;soffice --headless 在同一台机器上正常;完整环境变量、
 * Batch=true 都无效)。Windows 后端不支持 unipoll,只能在线程模式下找死锁点,所以要栈。
 *
 * 用法:node test/diag-hang.mjs <fixture 扩展名>   (例如 xlsx)
 * 输出:每个线程的栈,打到 stdout;调用方负责把它写成 annotation。
 * 定位并修复后连同 build.yml 里的诊断步骤一起删掉。
 */
import { spawnSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { HelperClient } from './protocol-client.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const kind = process.argv[2] ?? 'xlsx'
const helper = join(root, 'native', `${process.platform}-${process.arch}`, process.platform === 'win32' ? 'ncw-office-helper.exe' : 'ncw-office-helper')
const work = mkdtempSync(join(tmpdir(), 'ncw-diag-'))
const file = join(work, `diag.${kind}`)
copyFileSync(join(root, 'test-fixtures', `blank.${kind}`), file)

const client = await HelperClient.start({ helper, loPath: process.env.NCW_LO_PATH, workDir: join(work, 'helper') })
const opened = client.request('document.open', { path: file, format: kind }).then(() => 'opened', (error) => `failed: ${error.message}`)
const outcome = await Promise.race([opened, new Promise((r) => { setTimeout(() => { r('hung') }, 45_000) })])
console.log(`open ${kind}: ${outcome}`)

if (outcome === 'hung') {
  const cdb = [
    'C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe',
    'C:\\Program Files\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe'
  ].find((candidate) => existsSync(candidate))
  if (cdb === undefined) {
    console.log('cdb.exe not found on this runner; cannot dump stacks')
  } else {
    // .symfix:微软符号服务器(系统 DLL 的栈帧);LibreOffice DLL 只有导出符号,够看出卡在哪个模块哪个导出函数
    const result = spawnSync(cdb, ['-p', String(client.child.pid), '-c', `.symfix ${join(work, 'sym')};.reload;~*kn 25;qd`], {
      encoding: 'utf8',
      timeout: 240_000,
      maxBuffer: 64 * 1024 * 1024
    })
    const text = `${result.stdout ?? ''}${result.stderr ?? ''}`
    // 去掉符号加载的噪音行,只留线程头与栈帧
    console.log(text.split(/\r?\n/).filter((line) => /^\s*(#|\.|\d+\s+Id:|[0-9a-f]{2} [0-9a-f`]{8,})/i.test(line) || / Id: /.test(line)).join('\n'))
  }
}
client.child.kill('SIGKILL')
process.exit(0)
