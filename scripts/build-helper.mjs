#!/usr/bin/env node
/**
 * 构建 ncw-office-helper(LibreOfficeKit 承载进程),并把摘要登记进插件清单。
 *
 * 需求:插件清单里每个原生 target 都要带入口路径和 sha256(NextCoWork 安装器据此核对)。
 * 每次重新编译摘要都会变,手工抄一遍摘要迟早抄错 —— 抄错的症状是
 * 「装不上:digest does not match」。所以编译之后立刻写出 target 描述并登记。
 *
 * 用法:node scripts/build-helper.mjs
 *
 * 产出:
 *   native/<platform>-<arch>/ncw-office-helper[.exe]
 *   native/<platform>-<arch>/target.json   —— CI 的汇总步骤靠它合并四个平台
 *
 * ★ 只构建**当前机器**的平台/架构。另外的平台由 CI 在各自的机器上构建
 *   (.github/workflows/build.yml),再由 scripts/register-targets.mjs 汇总。
 * ★ 不打包 LibreOffice 本身:helper 运行时在自己旁边找随插件发行的 LibreOffice
 *   (`LibreOffice.app/Contents/Frameworks` / `libreoffice/program`),
 *   开发时用 `--lo-path=` 指向本机安装。
 */
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { registerTargets } from './register-targets.mjs'

const pluginRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const source = join(pluginRoot, 'native-src', 'ncw-office-helper.cxx')
const platform = process.platform
const arch = process.arch
const exe = platform === 'win32' ? 'ncw-office-helper.exe' : 'ncw-office-helper'
const entry = `native/${platform}-${arch}/${exe}`
const output = join(pluginRoot, entry)
mkdirSync(dirname(output), { recursive: true })

const warnings = ['-Wall', '-Wextra', '-Wno-unused-parameter']
const commands = {
  darwin: ['clang++', ['-std=c++17', '-O2', ...warnings, source, '-o', output]],
  linux: ['g++', ['-std=c++17', '-O2', ...warnings, source, '-o', output, '-ldl', '-pthread']],
  // /Fo 指到临时目录:cl 默认把 .obj 丢在当前目录,会混进 native-src
  win32: ['cl', ['/nologo', '/std:c++17', '/O2', '/EHsc', '/W3', source, `/Fe:${output}`, `/Fo:${join(tmpdir(), 'ncw-office-helper.obj')}`]]
}
const command = commands[platform]
if (command === undefined) {
  console.error(`✗ unsupported build platform: ${platform}`)
  process.exit(1)
}
const result = spawnSync(command[0], command[1], { stdio: 'inherit', cwd: join(pluginRoot, 'native-src') })
if (result.status !== 0) {
  console.error(`✗ ${command[0]} failed${result.error === undefined ? '' : `: ${result.error.message}`}`)
  process.exit(result.status ?? 1)
}

const sha256 = createHash('sha256').update(readFileSync(output)).digest('hex')
writeFileSync(join(dirname(output), 'target.json'), `${JSON.stringify({ platform, arch, entry, sha256 }, null, 2)}\n`)
const registered = registerTargets(pluginRoot)
console.log(`✓ ${entry}  sha256=${sha256.slice(0, 16)}…  (manifest now lists: ${registered.join(', ')})`)
