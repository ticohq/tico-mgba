#!/usr/bin/env python3
"""Settings definition and translations for the tico module of mgba.

Run after merging upstream. It builds tools/dump_core_options.c against the
core's own libretro_core_options.h, so tico/module/settings.json lists exactly
the options the libnx core reads (mgba_* keys, values and defaults), laid
out in tabs, followed by the overlay's own display and controls options.
Labels are translation keys; the strings go into tico/lang/*.json, taken from
tico's existing settings labels where an option already had one and otherwise
from the core's own translations. Choice labels stay English in settings.json;
the overlay translates them through settings_mgba_value_* keys.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICO = ROOT / "tico"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
# tico-nx's own strings label the options it already knew; optional
TICO_NX = Path(os.environ.get("TICO_NX_DIR", ROOT.parents[1] / "tico-nx"))

# Core option -> label key. Options tico already labelled keep their key (and
# tico's translations); the rest are named after the option.
LABEL_KEYS = {
    "mgba_gb_model": "settings_mgba_game_boy_model",
    "mgba_use_bios": "settings_mgba_use_bios_file",
    "mgba_skip_bios": "settings_mgba_skip_bios_intro",
    "mgba_gb_colors": "settings_mgba_default_palette",
    "mgba_gb_colors_preset": "settings_mgba_preset_palettes",
    "mgba_sgb_borders": "settings_mgba_sgb_borders",
    "mgba_color_correction": "settings_mgba_color_correction",
    "mgba_interframe_blending": "settings_mgba_interframe_blending",
    "mgba_audio_low_pass_filter": "settings_mgba_low_pass_filter",
    "mgba_audio_low_pass_range": "settings_mgba_filter_level",
    "mgba_allow_opposing_directions": "settings_mgba_allow_opposing_dir",
    "mgba_solar_sensor_level": "settings_mgba_solar_sensor",
    "mgba_force_gbp": "settings_mgba_force_gb_player",
    "mgba_idle_optimization": "settings_mgba_idle_optimization",
    "mgba_frameskip": "settings_mgba_method",
}

# Core options the frontend has no use for.
EXCLUDED: set[str] = set()

# (tab, [(section, [option keys])]). Every core option the dump lists must be
# placed or excluded; the overlay's own tabs are added around these. The tabs
# and sections are the ones tico already showed for mGBA.
LAYOUT = [
    ("settings_mgba_tab_system", [
        ("settings_mgba_section_system", ["mgba_gb_model", "mgba_use_bios", "mgba_skip_bios"]),
    ]),
    ("settings_mgba_tab_video", [
        ("settings_mgba_section_game_boy_options", ["mgba_gb_colors", "mgba_gb_colors_preset",
                                                    "mgba_sgb_borders"]),
        ("settings_mgba_section_enhancements", ["mgba_color_correction",
                                                "mgba_interframe_blending"]),
    ]),
    ("settings_mgba_tab_audio", [
        ("settings_mgba_section_audio_filter", ["mgba_audio_low_pass_filter",
                                                "mgba_audio_low_pass_range"]),
    ]),
    ("settings_mgba_tab_input", [
        ("settings_mgba_section_input", ["mgba_allow_opposing_directions",
                                         "mgba_solar_sensor_level", "mgba_force_gbp"]),
    ]),
    ("settings_mgba_tab_perf", [
        ("settings_mgba_section_optimization", ["mgba_idle_optimization"]),
        ("settings_mgba_section_frameskip", ["mgba_frameskip", "mgba_frameskip_threshold",
                                             "mgba_frameskip_interval"]),
    ]),
]

# Listed only while another option has a value, like the core's own menus.
DEPENDS_ON: dict[str, tuple[str, str]] = {
    "mgba_audio_low_pass_range": ("mgba_audio_low_pass_filter", "enabled"),
    "mgba_frameskip_threshold": ("mgba_frameskip", "auto_threshold"),
    "mgba_frameskip_interval": ("mgba_frameskip", "fixed_interval"),
}

# Switch buttons a GBA button (or the fast-forward hotkey) can sit on.
SWITCH_BUTTONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"),
                  ("ZL", "ZL"), ("ZR", "ZR"), ("Plus", "Plus"), ("Minus", "Minus"),
                  ("StickL", "Left stick"), ("StickR", "Right stick"), ("Up", "Up"),
                  ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("None", "Disabled")]

POSITIONS = [("hidden", "Hidden"), ("top_left", "Top left"), ("top_right", "Top right"),
             ("bottom_left", "Bottom left"), ("bottom_right", "Bottom right")]

# The overlay's own options: how the game is scaled, fast forward and the HUD.
# Shaders are picked in game (Settings > Shaders): tico cannot see the presets
# on the SD card.
OVERLAY_TAB = ("settings_mgba_tab_display", [
    ("settings_mgba_section_screen", [
        {"key": "display_mode", "label": "settings_mgba_display_mode", "type": "enum",
         "default": "Integer", "choices": [("Integer", "Integer"), ("Display", "Display")]},
        {"key": "display_size", "label": "settings_mgba_display_size", "type": "enum",
         "default": "Auto", "choices": [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"),
                                             ("Original", "Original"), ("1x", "1x"), ("2x", "2x"),
                                             ("Auto", "Auto")]},
    ]),
    ("settings_mgba_section_fast_forward", [
        {"key": "fast_forward_speed", "label": "settings_mgba_fast_forward_speed", "type": "enum",
         "default": "200", "choices": [("150", "150%"), ("200", "200%"), ("300", "300%"),
                                        ("400", "400%"), ("unlimited", "Unlimited")]},
        {"key": "fast_forward_mode", "label": "settings_mgba_fast_forward_mode", "type": "enum",
         "default": "hold", "choices": [("hold", "Hold"), ("toggle", "Toggle")]},
        {"key": "fast_forward_hotkey", "label": "settings_mgba_fast_forward_hotkey", "type": "enum",
         "default": "ZR", "choices": SWITCH_BUTTONS},
    ]),
    ("settings_mgba_section_hud", [
        {"key": "fps_counter_position", "label": "settings_mgba_fps_counter", "type": "enum",
         "default": "hidden", "choices": POSITIONS},
        {"key": "rendered_ir_position", "label": "settings_mgba_rendered_resolution",
         "type": "enum", "default": "hidden", "choices": POSITIONS},
    ]),
])

# The overlay's button mapping: GBA button -> Switch button, each on its
# namesake by default.
CONTROLS_TAB = ("settings_mgba_tab_controls", [
    ("settings_mgba_section_button_mapping", [
        {"key": key, "label": "settings_mgba_" + key, "type": "enum", "default": default,
         "choices": SWITCH_BUTTONS}
        for key, default in [
            ("map_a", "A"), ("map_b", "B"),
            ("map_l", "L"), ("map_r", "R"), ("map_start", "Plus"), ("map_select", "Minus"),
            ("map_up", "Up"), ("map_down", "Down"), ("map_left", "Left"), ("map_right", "Right"),
        ]
    ] + [
        {"key": "analog_dpad", "label": "settings_mgba_analog_dpad", "type": "bool",
         "default": "enabled"},
    ]),
])

# Labels the core does not define. key -> (en, de, es, fr, ja, pt, ru, zh)
LABELS = {
    # tico's own tab and section titles for mGBA; tico-nx's wording wins for
    # the languages it has, these fill German and Russian
    "settings_mgba_tab_system": ("System", "System", "Sistema", "Système", "システム", "Sistema",
                                 "Система", "系统"),
    "settings_mgba_tab_video": ("Video", "Video", "Vídeo", "Vidéo", "映像", "Vídeo", "Видео", "视频"),
    "settings_mgba_tab_audio": ("Audio", "Audio", "Audio", "Audio", "音声", "Áudio", "Звук", "音频"),
    "settings_mgba_tab_input": ("Input", "Eingabe", "Entrada", "Entrées", "入力", "Entrada",
                                "Ввод", "输入"),
    "settings_mgba_tab_perf": ("Perf.", "Leistung", "Rend.", "Perf.", "性能", "Desemp.",
                               "Производ.", "性能"),
    "settings_mgba_section_system": ("System", "System", "Sistema", "Système", "システム",
                                     "Sistema", "Система", "系统"),
    "settings_mgba_section_game_boy_options": ("Game Boy Options", "Game-Boy-Optionen",
                                               "Opciones de Game Boy", "Options Game Boy",
                                               "ゲームボーイ設定", "Opções do Game Boy",
                                               "Настройки Game Boy", "Game Boy 选项"),
    "settings_mgba_section_enhancements": ("Enhancements", "Verbesserungen", "Mejoras",
                                           "Améliorations", "画質向上", "Melhorias", "Улучшения",
                                           "增强"),
    "settings_mgba_section_audio_filter": ("Audio Filter", "Audiofilter", "Filtro de audio",
                                           "Filtre audio", "オーディオフィルター", "Filtro de áudio",
                                           "Аудиофильтр", "音频滤镜"),
    "settings_mgba_section_input": ("Input", "Eingabe", "Entrada", "Entrées", "入力", "Entrada",
                                    "Ввод", "输入"),
    "settings_mgba_section_optimization": ("Optimization", "Optimierung", "Optimización",
                                           "Optimisation", "最適化", "Otimização", "Оптимизация",
                                           "优化"),
    "settings_mgba_section_frameskip": ("Frameskip", "Frameskip", "Salto de fotogramas",
                                        "Saut d'images", "フレームスキップ", "Pulo de quadros",
                                        "Пропуск кадров", "跳帧"),
    "settings_mgba_tab_display": ("Display", "Anzeige", "Pantalla", "Affichage", "表示", "Tela",
                                    "Экран", "显示"),
    "settings_mgba_section_screen": ("Screen", "Bild", "Imagen", "Image", "画面", "Imagem",
                                       "Изображение", "画面"),
    "settings_mgba_section_hud": ("On-screen info", "Bildschirmanzeige", "Información en pantalla",
                                    "Affichage à l'écran", "画面表示", "Informações na tela",
                                    "Экранная информация", "屏幕信息"),
    "settings_mgba_display_mode": ("Display Mode", "Anzeigemodus", "Modo de pantalla",
                                     "Mode d'affichage", "表示モード", "Modo de exibição",
                                     "Режим отображения", "显示模式"),
    "settings_mgba_display_size": ("Size", "Größe", "Tamaño", "Taille", "サイズ", "Tamanho",
                                     "Размер", "尺寸"),
    "settings_mgba_fps_counter": ("FPS counter", "FPS-Zähler", "Contador de FPS",
                                    "Compteur de FPS", "FPSカウンター", "Contador de FPS",
                                    "Счётчик FPS", "帧率计数器"),
    "settings_mgba_rendered_resolution": ("Rendered resolution", "Gerenderte Auflösung",
                                            "Resolución renderizada", "Résolution de rendu",
                                            "描画解像度", "Resolução renderizada",
                                            "Разрешение рендеринга", "渲染分辨率"),
    "settings_mgba_section_fast_forward": ("Fast Forward", "Vorspulen", "Avance rápido",
                                             "Avance rapide", "早送り", "Avanço rápido",
                                             "Перемотка", "快进"),
    "settings_mgba_fast_forward_speed": ("Fast forward speed", "Vorspul-Geschwindigkeit",
                                           "Velocidad de avance rápido", "Vitesse d'avance rapide",
                                           "早送りの速度", "Velocidade do avanço rápido",
                                           "Скорость перемотки", "快进速度"),
    "settings_mgba_fast_forward_mode": ("Fast forward mode", "Vorspul-Modus",
                                          "Modo de avance rápido", "Mode d'avance rapide",
                                          "早送りモード", "Modo do avanço rápido",
                                          "Режим перемотки", "快进模式"),
    "settings_mgba_fast_forward_hotkey": ("Fast forward button", "Vorspul-Taste",
                                            "Botón de avance rápido", "Bouton d'avance rapide",
                                            "早送りボタン", "Botão do avanço rápido",
                                            "Кнопка перемотки", "快进按键"),
    "settings_mgba_tab_controls": ("Controls", "Steuerung", "Controles", "Commandes", "操作",
                                     "Controles", "Управление", "控制"),
    "settings_mgba_section_button_mapping": ("Button mapping", "Tastenbelegung",
                                               "Asignación de botones", "Attribution des boutons",
                                               "ボタン割り当て", "Mapeamento de botões",
                                               "Назначение кнопок", "按键映射"),
    "settings_mgba_map_a": ("A", "A", "A", "A", "A", "A", "A", "A"),
    "settings_mgba_map_b": ("B", "B", "B", "B", "B", "B", "B", "B"),
    "settings_mgba_map_l": ("L", "L", "L", "L", "L", "L", "L", "L"),
    "settings_mgba_map_r": ("R", "R", "R", "R", "R", "R", "R", "R"),
    "settings_mgba_map_start": ("Start", "Start", "Start", "Start", "スタート", "Start",
                                  "Start", "开始"),
    "settings_mgba_map_select": ("Select", "Select", "Select", "Select", "セレクト", "Select",
                                   "Select", "选择"),
    "settings_mgba_map_up": ("D-Pad Up", "Steuerkreuz oben", "Cruceta arriba",
                               "Croix haut", "十字キー上", "Direcional para cima",
                               "Крестовина вверх", "方向键上"),
    "settings_mgba_map_down": ("D-Pad Down", "Steuerkreuz unten", "Cruceta abajo",
                                 "Croix bas", "十字キー下", "Direcional para baixo",
                                 "Крестовина вниз", "方向键下"),
    "settings_mgba_map_left": ("D-Pad Left", "Steuerkreuz links", "Cruceta izquierda",
                                 "Croix gauche", "十字キー左", "Direcional para a esquerda",
                                 "Крестовина влево", "方向键左"),
    "settings_mgba_map_right": ("D-Pad Right", "Steuerkreuz rechts", "Cruceta derecha",
                                  "Croix droite", "十字キー右", "Direcional para a direita",
                                  "Крестовина вправо", "方向键右"),
    "settings_mgba_analog_dpad": ("Left stick as D-Pad", "Linker Stick als Steuerkreuz",
                                    "Stick izquierdo como cruceta",
                                    "Stick gauche comme croix directionnelle",
                                    "左スティックを十字キーとして使う",
                                    "Analógico esquerdo como direcional",
                                    "Левый стик как крестовина", "左摇杆作为方向键"),
    # options tico did not have before; the core only describes them in English
}

# Choice labels the core does not translate. English -> (de, es, fr, ja, pt, ru, zh)
CHOICES = {
    "Disabled": ("Deaktiviert", "Desactivado", "Désactivé", "無効", "Desativado", "Выключено",
                 "禁用"),
    "Integer": ("Ganzzahlig", "Entero", "Entier", "整数倍", "Inteiro", "Целочисленный", "整数"),
    "Display": ("Anzeige", "Pantalla", "Écran", "画面", "Tela", "Экран", "屏幕"),
    "Stretch": ("Strecken", "Estirar", "Étirer", "引き伸ばし", "Esticar", "Растянуть", "拉伸"),
    "Original": ("Original", "Original", "Original", "オリジナル", "Original", "Оригинал", "原始"),
    "Auto": ("Auto", "Auto", "Auto", "自動", "Auto", "Авто", "自动"),
    "Unlimited": ("Unbegrenzt", "Ilimitado", "Illimité", "無制限", "Ilimitado", "Без ограничений",
                  "无限制"),
    "Hold": ("Halten", "Mantener", "Maintenir", "長押し", "Segurar", "Удерживать", "按住"),
    "Toggle": ("Umschalten", "Alternar", "Basculer", "切り替え", "Alternar", "Переключать", "切换"),
    "Right stick": ("Rechter Stick", "Stick derecho", "Stick droit", "右スティック", "Analógico direito",
                    "Правый стик", "右摇杆"),
    "Left stick": ("Linker Stick", "Stick izquierdo", "Stick gauche", "左スティック", "Analógico esquerdo",
                   "Левый стик", "左摇杆"),
    "Plus": ("Plus", "Más", "Plus", "プラス", "Mais", "Плюс", "加号"),
    "Minus": ("Minus", "Menos", "Moins", "マイナス", "Menos", "Минус", "减号"),
    "Up": ("Oben", "Arriba", "Haut", "上", "Cima", "Вверх", "上"),
    "Down": ("Unten", "Abajo", "Bas", "下", "Baixo", "Вниз", "下"),
    "Left": ("Links", "Izquierda", "Gauche", "左", "Esquerda", "Влево", "左"),
    "Right": ("Rechts", "Derecha", "Droite", "右", "Direita", "Вправо", "右"),
    "Hidden": ("Ausgeblendet", "Oculto", "Masqué", "非表示", "Oculto", "Скрыто", "隐藏"),
    "Top left": ("Oben links", "Arriba a la izquierda", "En haut à gauche", "左上",
                 "Superior esquerdo", "Сверху слева", "左上"),
    "Top right": ("Oben rechts", "Arriba a la derecha", "En haut à droite", "右上",
                  "Superior direito", "Сверху справа", "右上"),
    "Bottom left": ("Unten links", "Abajo a la izquierda", "En bas à gauche", "左下",
                    "Inferior esquerdo", "Снизу слева", "左下"),
    "Bottom right": ("Unten rechts", "Abajo a la derecha", "En bas à droite", "右下",
                     "Inferior direito", "Снизу справа", "右下"),
}

# Options the libretro core reads only when a game loads (_reloadSettings),
# though their descriptions do not say so.
RESTART_KEYS = {"mgba_gb_model", "mgba_use_bios", "mgba_skip_bios", "mgba_sgb_borders",
                "mgba_idle_optimization", "mgba_force_gbp"}
RESTART_SUFFIX = re.compile(r"\s*\((Restart Required|Reload Core|[^)]*[Nn]eustart[^)]*|[^)]*[Rr]einici[^)]*|"
                            r"[^)]*[Rr]edémarr[^)]*|[^)]*再起動[^)]*|[^)]*перезапуск[^)]*|"
                            r"[^)]*重启[^)]*|[^)]*[Rr]einicializa[^)]*)\)")


def value_key(label: str) -> str:
    """tico_config.cpp's ValueKey: settings_mgba_value_ + label as a slug."""
    return "settings_mgba_value_" + "_".join(re.findall(r"[a-z0-9]+", label.lower()))


def clean_label(text: str) -> str:
    return RESTART_SUFFIX.sub("", text).lstrip("> ").strip()


def english_choice(value: str, label: str) -> str:
    # the core leaves on/off style values unlabelled
    return label.capitalize() if label == value and value in ("disabled", "enabled") else label


def dump_options() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "dump_core_options"
        # the libnx build's colour defines gate colour correction and blending
        subprocess.run(["cc", "-std=gnu11", "-w", "-DCOLOR_16_BIT", "-DCOLOR_5_6_5",
                        "-I", str(ROOT / "src/platform/libretro"),
                        "-I", str(ROOT / "libretro-build/include"),
                        "-o", str(exe), str(Path(__file__).with_name("dump_core_options.c"))],
                       check=True)
        return json.loads(subprocess.run([str(exe)], check=True, capture_output=True,
                                         text=True).stdout)


def label_key(key: str) -> str:
    return LABEL_KEYS.get(key, "settings_mgba_" + key.removeprefix("mgba_"))


def build_settings(dump: dict) -> dict:
    core = {o["key"]: o for o in dump["en"]}
    placed = {key for _, sections in LAYOUT for _, keys in sections for key in keys}
    missing = sorted(set(core) - placed - EXCLUDED)
    if missing:
        raise SystemExit(f"core options not placed in LAYOUT: {missing}")

    tabs = []
    for tab, sections in LAYOUT:
        out_sections = []
        for title, keys in sections:
            options = []
            for key in keys:
                source = core[key]
                values = [v for v, _ in source["values"]]
                option = {"key": key, "label": label_key(key)}
                if sorted(values) == ["disabled", "enabled"]:
                    option.update(type="bool", default=source["default"])
                else:
                    option.update(type="enum", default=source["default"], choices=[
                        {"label": english_choice(v, l), "value": v} for v, l in source["values"]])
                if RESTART_SUFFIX.search(source["desc"]) or key in RESTART_KEYS:
                    option["restart"] = True
                if key in DEPENDS_ON:
                    on, value = DEPENDS_ON[key]
                    option["depends_on"] = {"key": on, "value": value}
                options.append(option)
            out_sections.append({"title": title, "options": options})
        tabs.append({"name": tab, "sections": out_sections})

    def overlay_tab(definition):
        tab, sections = definition
        return {"name": tab, "sections": [
            {"title": title, "options": [
                {**o, "choices": [{"label": l, "value": v} for v, l in o["choices"]]}
                if "choices" in o else dict(o)
                for o in options]}
            for title, options in sections]}

    tabs.insert(1, overlay_tab(OVERLAY_TAB))
    tabs.append(overlay_tab(CONTROLS_TAB))

    return {
        "core_id": "mgba",
        "display_name": "mGBA",
        "config_file": "mgba.jsonc",
        "slugs": ["gba"],
        "bool_true_value": "enabled",
        "bool_false_value": "disabled",
        "tabs": tabs,
    }


def tico_owned(key: str) -> bool:
    """Labels tico itself defines, whose wording tico keeps."""
    return key in LABEL_KEYS.values() or key.startswith(("settings_mgba_tab_",
                                                          "settings_mgba_section_"))


def build_strings(dump: dict, settings: dict,
                  existing: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    strings: dict[str, dict[str, str]] = {lang: {} for lang in LANGUAGES}
    english = {o["key"]: o for o in dump["en"]}
    for lang in LANGUAGES:
        out = strings[lang]
        index = LANGUAGES.index(lang)
        for option in dump[lang]:
            if option["key"] in EXCLUDED:
                continue
            key, en = option["key"], english[option["key"]]
            out[label_key(key)] = clean_label(option["desc"])
            for (value, label), (_, en_label) in zip(option["values"], en["values"]):
                en_text = english_choice(value, en_label)
                if label != en_label:
                    out[value_key(en_text)] = label
        for key, texts in LABELS.items():
            out[key] = texts[index]
        if lang != "en":
            for choice, translations in CHOICES.items():
                out[value_key(choice)] = translations[index - 1]
        if lang == "en":
            for key in list(out):
                if key.startswith("settings_mgba_value_"):
                    del out[key]
    # tico's own labels keep tico's wording: from tico-nx when it is checked
    # out, otherwise as the language files already have them
    for lang in LANGUAGES:
        path = TICO_NX / "assets/lang" / f"{lang}.json"
        source = json.loads(path.read_text()) if path.exists() else existing[lang]
        for key, value in source.items():
            if tico_owned(key):
                strings[lang][key] = value
    used = set()

    def walk(node):
        if isinstance(node, dict):
            for field in ("label", "name", "title"):
                if isinstance(node.get(field), str) and node[field].startswith("settings_"):
                    used.add(node[field])
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(settings)
    unlabelled = sorted(used - set(strings["en"]))
    if unlabelled:
        raise SystemExit(f"labels without English text: {unlabelled}")
    return strings


def main() -> None:
    dump = dump_options()
    settings = build_settings(dump)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    current_files = {lang: json.loads((LANG_DIR / f"{lang}.json").read_text())
                     if (LANG_DIR / f"{lang}.json").exists() else {} for lang in LANGUAGES}
    for lang, strings in build_strings(dump, settings, current_files).items():
        path = LANG_DIR / f"{lang}.json"
        current = {k: v for k, v in current_files[lang].items()
                   if not k.startswith("settings_mgba_")}
        current.update(dict(sorted(strings.items())))
        path.write_text(json.dumps(current, indent=4, ensure_ascii=False) + "\n")
    print(f"wrote {SETTINGS.relative_to(ROOT)} and {len(LANGUAGES)} language files")


if __name__ == "__main__":
    main()
