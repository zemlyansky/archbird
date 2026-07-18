{
  "includes": ["csrc/archbird.gypi"],
  "targets": [
    {
      "target_name": "_native",
      "sources": ["<@(archbird_sources)"],
      "include_dirs": ["<@(archbird_include_dirs)"],
      "defines": ["<@(archbird_defines)"],
      "conditions": [
        ["OS == 'win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": ["/std:c11"],
              "WarningLevel": 4
            }
          }
        }, {
          "cflags_c": [
            "-std=c11",
            "-fvisibility=hidden",
            "-Wno-cast-function-type",
            "-Wno-overlength-strings"
          ]
        }]
      ]
    }
  ]
}
