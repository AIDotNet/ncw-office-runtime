#!/usr/bin/env node
/**
 * 打一个**带完整 LibreOffice 运行时**的单平台插件包。
 *
 * ## 为了什么需求建的
 *
 * 办公插件在用户安装插件时就带上 LibreOffice(不装进 NextCoWork 主程序,也不要求用户另装)。
 * 一份运行时是几百 MB、上万个文件,四个平台放进一个包就是好几 GB,所以**每个平台一个包**,
 * 清单里只列本平台那一个 target。
 *
 * NextCoWork 安装器(`src/main/plugin/native-installer.ts`)按这里写出的**文件索引**逐个核对
 * 运行时:多一个文件、少一个文件、内容不对都整包拒装;可执行位与包内符号链接也按索引还原
 * (ZIP 里永远不放链接)。所以索引必须覆盖 `native/<target>/` 下的每一个文件。
 *
 * ## 用法
 *
 *   node scripts/build-helper.mjs                       # 先编译本机 helper
 *   node scripts/package-plugin.mjs --lo-path=<program 目录>
 *
 * `--lo-path` 与 helper / 测试用的是同一个值(macOS 是 `LibreOffice.app/Contents/Frameworks`,
 * 其余平台是 LibreOffice 的 `program` 目录);脚本据此找到整个安装的根目录再复制。
 *
 * 产出:
 *   build/ncw.office-runtime/                                   解开的插件目录(可直接按目录安装)
 *   build/ncw.office-runtime-<version>-<platform>-<arch>.zip    发布用的包
 *
 * ## 不做的
 *
 * - 不做代码签名 / 公证:LibreOffice 本身带 TDF 的签名,原样复制;helper 的签名、以及
 *   NextCoWork 这一侧是否要求签名,是发布流程的事。
 * - 不替你做许可证审查:会把 LibreOffice 自带的许可证文件复制进 `licenses/libreoffice/`,
 *   但分发义务(对应源码、商标)需要人为确认,见 `licenses/NOTICE.txt`。
 */
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { cpSync, existsSync, lstatSync, mkdirSync, readdirSync, readFileSync, readlinkSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { basename, dirname, join, posix, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const platform = process.platform
const arch = process.arch
const targetName = `${platform}-${arch}`
const exe = platform === 'win32' ? 'ncw-office-helper.exe' : 'ncw-office-helper'
const PLUGIN_DIR = 'ncw.office-runtime'

function fail(message) {
  console.error(`✗ ${message}`)
  process.exit(1)
}

function option(name) {
  const prefix = `--${name}=`
  const arg = process.argv.slice(2).find((item) => item.startsWith(prefix))
  return arg === undefined ? undefined : arg.slice(prefix.length)
}

/**
 * 从 `--lo-path` 推出安装根目录,以及它在包里的名字(helper 按这个名字找,见 libreOfficePath())。
 * ★ macOS 要整个 .app:Frameworks 里的库引用 Resources、PlugIns,且 .app 的签名覆盖整个包。
 */
function runtimeRoot(loPath) {
  const absolute = resolve(loPath)
  if (platform === 'darwin') {
    const app = resolve(absolute, '..', '..')
    if (basename(app) !== 'LibreOffice.app' || basename(absolute) !== 'Frameworks') {
      fail(`on macOS --lo-path must be .../LibreOffice.app/Contents/Frameworks (got ${loPath})`)
    }
    return { source: app, name: 'LibreOffice.app' }
  }
  if (basename(absolute) !== 'program') fail(`--lo-path must be LibreOffice's program directory (got ${loPath})`)
  return { source: resolve(absolute, '..'), name: 'libreoffice' }
}

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex')

/**
 * 运行时的文件索引。路径都相对插件根、`/` 分隔、以 `native/<target>/` 开头 ——
 * 形状与校验规则见 NextCoWork 的 `native-installer.ts` 的 parsePayloadIndex。
 */
function buildIndex(pluginRoot, scope, skip) {
  const files = []
  const symlinks = []
  const walk = (rel) => {
    for (const entry of readdirSync(join(pluginRoot, ...rel.split('/')), { withFileTypes: true })) {
      const child = `${rel}/${entry.name}`
      if (child === skip) continue
      const absolute = join(pluginRoot, ...child.split('/'))
      const info = lstatSync(absolute)
      if (info.isSymbolicLink()) {
        const target = readlinkSync(absolute)
        const resolved = posix.normalize(posix.join(posix.dirname(child), target.replaceAll('\\', '/')))
        // 与安装器同一条规则:只收相对链接,且落在本 target 目录之内
        if (target.startsWith('/') || /^[A-Za-z]:/.test(target) || !resolved.startsWith(`${scope}/`)) {
          fail(`symlink ${child} -> ${target} does not stay inside ${scope}/`)
        }
        symlinks.push({ path: child, target })
      } else if (info.isDirectory()) {
        walk(child)
      } else if (info.isFile()) {
        files.push({ path: child, size: info.size, sha256: sha256(readFileSync(absolute)), executable: (info.mode & 0o111) !== 0 })
      } else {
        fail(`unsupported file type in the runtime: ${child}`)
      }
    }
  }
  walk(scope)
  const byPath = (a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0)
  return { format: 1, files: files.sort(byPath), symlinks: symlinks.sort(byPath) }
}

/** 所有普通文件(相对 build/),给 zip 用 —— 链接不进归档,由索引在安装时重建 */
function regularFiles(base, rel, out = []) {
  for (const entry of readdirSync(join(base, rel), { withFileTypes: true })) {
    const child = `${rel}/${entry.name}`
    if (entry.isDirectory()) regularFiles(base, child, out)
    else if (entry.isFile()) out.push(child)
  }
  return out
}

// ─────────────────────────── 主流程 ───────────────────────────

const loPath = option('lo-path') ?? process.env.NCW_LO_PATH
if (loPath === undefined || loPath === '') fail('pass --lo-path=<LibreOffice program directory> (or set NCW_LO_PATH)')
const runtime = runtimeRoot(loPath)
if (!existsSync(runtime.source)) fail(`LibreOffice not found at ${runtime.source}`)

const descriptorPath = join(root, 'native', targetName, 'target.json')
if (!existsSync(descriptorPath)) fail(`no helper build for ${targetName}; run node scripts/build-helper.mjs first`)
const built = JSON.parse(readFileSync(descriptorPath, 'utf8'))
const helperSource = join(root, built.entry)
if (sha256(readFileSync(helperSource)) !== built.sha256) fail(`${built.entry} does not match its target.json; rebuild it`)

const build = join(root, 'build')
const out = join(build, PLUGIN_DIR)
rmSync(out, { recursive: true, force: true })
mkdirSync(out, { recursive: true })

// 插件本体:逻辑侧入口、许可证。native/ 只放本平台
cpSync(join(root, 'dist'), join(out, 'dist'), { recursive: true })
cpSync(join(root, 'licenses'), join(out, 'licenses'), { recursive: true })
const scope = `native/${targetName}`
const scopeDir = join(out, ...scope.split('/'))
mkdirSync(scopeDir, { recursive: true })
cpSync(helperSource, join(scopeDir, exe))

console.log(`… copying ${runtime.source} (this takes a while)`)
/*
  ★ verbatimSymlinks:不加的话 Node 会把相对链接改写成指向复制源的**绝对**路径 ——
  装到用户机器上,那些链接全部指向 CI 机器上早已不存在的目录,LibreOffice 起不来。
*/
cpSync(runtime.source, join(scopeDir, runtime.name), { recursive: true, verbatimSymlinks: true })

// LibreOffice 自带的许可证文件,复制一份到插件的 licenses/ 下,安装后在插件目录里就能找到
const licenseSource = platform === 'darwin' ? join(runtime.source, 'Contents', 'Resources') : runtime.source
const licenseOut = join(out, 'licenses', 'libreoffice')
mkdirSync(licenseOut, { recursive: true })
let licenseCount = 0
for (const name of ['LICENSE', 'LICENSE.html', 'LICENSE.txt', 'license.txt', 'NOTICE', 'CREDITS.fodt']) {
  const file = join(licenseSource, name)
  if (existsSync(file) && statSync(file).isFile()) { cpSync(file, join(licenseOut, name)); licenseCount += 1 }
}
if (licenseCount === 0) fail(`found no LibreOffice license files under ${licenseSource}`)

const indexPath = `${scope}/payload.json`
const index = buildIndex(out, scope, indexPath)
const indexText = `${JSON.stringify(index)}\n`
writeFileSync(join(out, ...indexPath.split('/')), indexText)

// 清单:只列本平台一个 target,带入口与索引的摘要
const manifest = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'))
delete manifest.scripts
const component = manifest.nativeComponents.find((item) => item.id === 'libreoffice')
// 随包带了 LibreOffice:许可证表达式要把它的 LGPL 部分也写上(各第三方组件见 licenses/libreoffice/)
component.license.spdx = 'Apache-2.0 AND MPL-2.0 AND LGPL-3.0-or-later'
component.targets = [{
  platform, arch, entry: `${scope}/${exe}`, sha256: built.sha256,
  payload: { index: indexPath, sha256: sha256(indexText) }
}]
writeFileSync(join(out, 'package.json'), `${JSON.stringify(manifest, null, 2)}\n`)

// 打 zip。只放普通文件(没有目录条目,也没有链接)
const zipName = `${PLUGIN_DIR}-${manifest.version}-${targetName}.zip`
const zipPath = join(build, zipName)
rmSync(zipPath, { force: true })
const list = regularFiles(build, PLUGIN_DIR)
console.log(`… zipping ${list.length} files`)
const result = platform === 'win32'
  // Windows 自带 bsdtar,-a 按扩展名写 zip;Windows 的运行时没有链接,不必挑文件
  ? spawnSync('tar', ['-a', '-c', '-f', zipPath, PLUGIN_DIR], { cwd: build, stdio: ['ignore', 'inherit', 'inherit'] })
  : spawnSync('zip', ['-q', '-X', '-@', zipPath], { cwd: build, input: list.join('\n'), stdio: ['pipe', 'inherit', 'inherit'] })
if (result.status !== 0) fail(`creating ${zipName} failed`)

const zipBytes = statSync(zipPath).size
const expanded = index.files.reduce((sum, file) => sum + file.size, 0)
console.log(`✓ ${zipName}`)
console.log(`  zip ${(zipBytes / 1048576).toFixed(0)} MiB · expanded ${(expanded / 1048576).toFixed(0)} MiB · ${index.files.length} files · ${index.symlinks.length} symlinks`)
console.log(`  sha256 ${sha256(readFileSync(zipPath))}`)
