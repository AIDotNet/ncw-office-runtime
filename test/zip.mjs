/**
 * 最小 zip 读取 —— 只给测试用来检查保存出的 OOXML(docx/xlsx/pptx 都是 zip)。
 *
 * 需求:有些改动从引擎的文本查询里看不出来(段落样式、宏项目是否保留),必须直接看
 * 保存出来的文件。仓库不引第三方依赖,Node 自带 zlib 能解 deflate,这里只补中央目录解析。
 * 只支持 stored(0) 与 deflate(8),不支持 zip64 —— 测试样本都很小。
 */
import { readFileSync } from 'node:fs'
import { inflateRawSync } from 'node:zlib'

export function readZipEntry(path, name) {
  const buf = readFileSync(path)
  // 中央目录结束记录:从尾部往前找签名 0x06054b50
  let eocd = -1
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) { eocd = i; break }
  }
  if (eocd < 0) throw new Error(`${path}: not a zip file`)
  const entries = buf.readUInt16LE(eocd + 10)
  let offset = buf.readUInt32LE(eocd + 16)
  for (let n = 0; n < entries; n++) {
    if (buf.readUInt32LE(offset) !== 0x02014b50) throw new Error(`${path}: bad central directory`)
    const method = buf.readUInt16LE(offset + 10)
    const compressedSize = buf.readUInt32LE(offset + 20)
    const nameLength = buf.readUInt16LE(offset + 28)
    const extraLength = buf.readUInt16LE(offset + 30)
    const commentLength = buf.readUInt16LE(offset + 32)
    const localOffset = buf.readUInt32LE(offset + 42)
    const entryName = buf.subarray(offset + 46, offset + 46 + nameLength).toString('utf8')
    if (entryName === name) {
      const localNameLength = buf.readUInt16LE(localOffset + 26)
      const localExtraLength = buf.readUInt16LE(localOffset + 28)
      const start = localOffset + 30 + localNameLength + localExtraLength
      const data = buf.subarray(start, start + compressedSize)
      if (method === 0) return data.toString('utf8')
      if (method === 8) return inflateRawSync(data).toString('utf8')
      throw new Error(`${path}: unsupported compression method ${method}`)
    }
    offset += 46 + nameLength + extraLength + commentLength
  }
  return null
}

/** docx 的段落列表:[{ style, text }]。只取 w:pStyle 与各 w:t 的拼接,够测试用 */
export function docxParagraphs(path) {
  const xml = readZipEntry(path, 'word/document.xml')
  if (xml === null) throw new Error(`${path}: no word/document.xml`)
  return [...xml.matchAll(/<w:p[ >][\s\S]*?<\/w:p>/g)].map(([p]) => ({
    style: /<w:pStyle w:val="([^"]*)"/.exec(p)?.[1] ?? '',
    text: [...p.matchAll(/<w:t(?: [^>]*)?>([^<]*)<\/w:t>/g)].map((m) => m[1]).join('')
  }))
}
