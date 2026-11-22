package com.vconvertercrew.vconverter.script

/**
 * Single source of truth for which file extensions count as scripts.
 *
 * - [LUA]: Psych Engine Lua scripts.
 * - [HSCRIPT]: plain HScript files (Psych Engine `.hx`/`.hxs`/`.hscript`, Codename Engine `.hsc`).
 * - [HXC]: V-Slice (Polymod) script classes.
 *
 * All helpers are case-insensitive on the extension and accept `/` or `\` separated paths.
 */
object ScriptExtensions {
    val LUA: Set<String> = setOf("lua")
    val HSCRIPT: Set<String> = setOf("hx", "hxs", "hsc", "hscript")
    val HXC: Set<String> = setOf("hxc")

    /** Scripts a Psych Engine mod can ship: Lua and HScript. */
    val PSYCH_SOURCES: Set<String> = LUA + HSCRIPT

    val ALL: Set<String> = LUA + HSCRIPT + HXC

    fun of(path: String): String = path.substringAfterLast('/').substringAfterLast('\\').substringAfterLast('.', "").lowercase()

    fun isLua(path: String): Boolean = of(path) in LUA

    fun isHScript(path: String): Boolean = of(path) in HSCRIPT

    fun isHxc(path: String): Boolean = of(path) in HXC

    fun isScript(path: String): Boolean = of(path) in ALL
}
