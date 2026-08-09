// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "SceneRuntime",
    platforms: [
        .macOS("14.2"),
    ],
    products: [
        .library(
            name: "SceneRuntimeBridge",
            type: .static,
            targets: ["SceneRuntimeBridge"]
        ),
        .library(
            name: "SceneAudio",
            targets: ["SceneAudio"]
        ),
    ],
    targets: [
        .target(
            name: "SceneJSON",
            path: "Sources/SceneJSON",
            publicHeadersPath: "."
        ),
        .target(
            name: "CWEGlslang",
            path: "Sources/CWEGlslang",
            sources: [
                "glslang/GenericCodeGen",
                "glslang/MachineIndependent",
                "glslang/OSDependent/Unix",
                "SPIRV",
            ],
            publicHeadersPath: ".",
            cxxSettings: [
                .define("ENABLE_SPIRV"),
                .define("GLSLANG_OSINCLUDE_UNIX"),
            ]
        ),
        .target(
            name: "CWESPIRVCross",
            path: "Sources/CWESPIRVCross",
            sources: [
                "spirv_cfg.cpp",
                "spirv_cross.cpp",
                "spirv_cross_parsed_ir.cpp",
                "spirv_glsl.cpp",
                "spirv_parser.cpp",
            ],
            publicHeadersPath: ".",
            cxxSettings: [
                .unsafeFlags(["-Wno-deprecated-this-capture"]),
            ]
        ),
        .target(
            name: "CLZ4",
            path: "Sources/CLZ4",
            publicHeadersPath: "include"
        ),
        .target(
            name: "CQuickJSNG",
            path: "Sources/CQuickJSNG",
            sources: [
                "dtoa.c",
                "libregexp.c",
                "libunicode.c",
                "quickjs.c",
            ],
            publicHeadersPath: ".",
            cSettings: [
                .define("_GNU_SOURCE"),
                .define("QUICKJS_NG_BUILD"),
            ]
        ),
        .target(
            name: "SceneCore",
            dependencies: ["CLZ4", "SceneJSON"],
            path: "Sources/SceneCore",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneModel",
            dependencies: ["SceneCore", "SceneJSON"],
            path: "Sources/SceneModel",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneModelTestSupport",
            dependencies: ["SceneCore", "SceneModel"],
            path: "Sources/SceneModelTestSupport",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneParticle",
            path: "Sources/SceneParticle",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneParticleTestSupport",
            dependencies: ["SceneParticle"],
            path: "Sources/SceneParticleTestSupport",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneScript",
            dependencies: ["CQuickJSNG", "SceneModel"],
            path: "Sources/SceneScript",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneScriptTestSupport",
            dependencies: ["SceneScript", "SceneJSON"],
            path: "Sources/SceneScriptTestSupport",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneGraph",
            dependencies: ["SceneModel", "SceneScript"],
            path: "Sources/SceneGraph",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneText",
            path: "Sources/SceneText",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreGraphics"),
                .linkedFramework("CoreText"),
            ]
        ),
        .target(
            name: "SceneTextTestSupport",
            dependencies: ["SceneText"],
            path: "Sources/SceneTextTestSupport",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneFrameGraph",
            dependencies: ["SceneCore", "SceneModel", "SceneGraph", "SceneParticle"],
            path: "Sources/SceneFrameGraph",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneShader",
            dependencies: ["SceneCore", "CWEGlslang", "CWESPIRVCross", "SceneJSON"],
            path: "Sources/SceneShader",
            publicHeadersPath: "include"
        ),
        .target(
            name: "SceneGL",
            dependencies: ["SceneCore", "SceneFrameGraph", "SceneShader", "SceneText"],
            path: "Sources/SceneGL",
            publicHeadersPath: "include",
            cxxSettings: [
                .define("GL_SILENCE_DEPRECATION"),
            ],
            linkerSettings: [
                .linkedFramework("OpenGL"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("ImageIO"),
                .linkedFramework("AVFoundation"),
            ]
        ),
        .target(
            name: "SceneGLTestSupport",
            dependencies: ["SceneGL", "SceneText"],
            path: "Sources/SceneGLTestSupport",
            publicHeadersPath: "include",
            cxxSettings: [.define("GL_SILENCE_DEPRECATION")]
        ),
        .target(
            name: "SceneRuntimeBridge",
            dependencies: [
                "SceneCore", "SceneModel", "SceneGraph", "SceneFrameGraph", "SceneShader", "SceneGL",
            ],
            path: "Sources/SceneRuntimeBridge",
            publicHeadersPath: "include"
        ),
        .target(
            name: "CSceneAudioRealtime",
            path: "Sources/CSceneAudioRealtime",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreAudio"),
            ]
        ),
        .target(
            name: "SceneAudio",
            dependencies: ["CSceneAudioRealtime", "SceneRuntimeBridge"],
            path: "Sources/SceneAudio",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("Accelerate"),
            ]
        ),
        .testTarget(
            name: "SceneCoreTests",
            dependencies: ["SceneRuntimeBridge"],
            path: "Tests/SceneCoreTests"
        ),
        .testTarget(
            name: "SceneGLTests",
            dependencies: ["SceneRuntimeBridge", "SceneGLTestSupport"],
            path: "Tests/SceneGLTests",
            swiftSettings: [
                .unsafeFlags(["-Xcc", "-DGL_SILENCE_DEPRECATION"]),
            ]
        ),
        .testTarget(
            name: "SceneModelTests",
            dependencies: ["SceneRuntimeBridge", "SceneModelTestSupport"],
            path: "Tests/SceneModelTests"
        ),
        .testTarget(
            name: "SceneParticleTests",
            dependencies: ["SceneParticleTestSupport"],
            path: "Tests/SceneParticleTests"
        ),
        .testTarget(
            name: "SceneGraphTests",
            dependencies: ["SceneRuntimeBridge"],
            path: "Tests/SceneGraphTests"
        ),
        .testTarget(
            name: "SceneFrameGraphTests",
            dependencies: ["SceneRuntimeBridge"],
            path: "Tests/SceneFrameGraphTests"
        ),
        .testTarget(
            name: "SceneScriptTests",
            dependencies: ["SceneScriptTestSupport"],
            path: "Tests/SceneScriptTests"
        ),
        .testTarget(
            name: "SceneTextTests",
            dependencies: ["SceneTextTestSupport"],
            path: "Tests/SceneTextTests",
            resources: [.copy("Fixtures")]
        ),
        .testTarget(
            name: "SceneAudioTests",
            dependencies: ["SceneAudio", "SceneRuntimeBridge"],
            path: "Tests/SceneAudioTests"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
