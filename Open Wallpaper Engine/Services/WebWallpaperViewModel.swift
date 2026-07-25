//
//  WebWallpaperViewModel.swift
//  Open Wallpaper Engine
//
//  Created by Toby on 2023/8/28.
//

import WebKit
import SwiftUI
import Combine
import Foundation

@MainActor
class WebWallpaperViewModel: NSObject, ObservableObject, WKNavigationDelegate {
    let screenId: String
    var currentWallpaper: WEWallpaper

    /// Last media-policy JavaScript error. Keep this observable so a host UI
    /// can surface a failed autoplay/mute operation instead of treating it as
    /// a successful policy application.
    @Published private(set) var mediaPolicyIssue: String?
    
    var fileUrl: URL {
        currentWallpaper.wallpaperDirectory.appending(path: currentWallpaper.project.file)
    }
    
    var readAccessURL: URL {
        currentWallpaper.wallpaperDirectory
    }

    private weak var attachedWebView: WKWebView?
    private var policyPaused = false
    private var appliedPaused: Bool?
    private var appliedPolicyMuted: Bool?
    private var appliedPolicyVolume: Float?
    private var javascriptIssues: [String: String] = [:]
    private var cancellables = Set<AnyCancellable>()
    
    init(wallpaper: WEWallpaper, screenId: String) {
        self.screenId = screenId
        self.currentWallpaper = wallpaper
        super.init()
        policyPaused = AppDelegate.shared.wallpaperViewModel.effectivePlayRate == 0

        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        wallpaperViewModel.$effectivePlayRate
            .receive(on: DispatchQueue.main)
            .sink { [weak self] rate in self?.setPolicyPaused(rate == 0) }
            .store(in: &cancellables)
        wallpaperViewModel.$effectivePlayVolume
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconcileMediaPolicy() }
            .store(in: &cancellables)
        AppDelegate.shared.globalSettingsViewModel.$settings
            .map(\.audioOutput)
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconcileMediaPolicy() }
            .store(in: &cancellables)
        AppDelegate.shared.sceneAudioOwnerCoordinator.$ownerScreenId
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconcileMediaPolicy() }
            .store(in: &cancellables)
        wallpaperViewModel.$scenePropertyPersistence
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyUserProperties() }
            .store(in: &cancellables)
        registerWebPropertyCatalog()
    }
    
    deinit {
        cancellables.removeAll()
    }

    func attach(_ webView: WKWebView) {
        guard attachedWebView !== webView else { return }
        attachedWebView = webView
        appliedPaused = nil
        appliedPolicyMuted = nil
        appliedPolicyVolume = nil
        reconcileMediaPolicy(force: true)
        applyUserProperties()
    }

    func updateWallpaper(_ wallpaper: WEWallpaper) {
        guard wallpaper.scenePropertyIdentity != currentWallpaper.scenePropertyIdentity ||
                wallpaper.project != currentWallpaper.project else { return }
        currentWallpaper = wallpaper
        registerWebPropertyCatalog()
        applyUserProperties()
    }

    func detach(_ webView: WKWebView) {
        guard attachedWebView === webView else { return }
        attachedWebView = nil
        appliedPaused = nil
        appliedPolicyMuted = nil
        appliedPolicyVolume = nil
    }

    private func setPolicyPaused(_ paused: Bool) {
        guard policyPaused != paused else { return }
        policyPaused = paused
        reconcileMediaPolicy()
    }

    private var shouldMuteAudio: Bool {
        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        let settings = AppDelegate.shared.globalSettingsViewModel.settings
        let isAudibleOwner = AppDelegate.shared.sceneAudioOwnerCoordinator.isAudible(screenId: screenId)
        return wallpaperViewModel.effectivePlayVolume <= 0 ||
            !settings.audioOutput ||
            !isAudibleOwner
    }

    private func reconcileMediaPolicy(force: Bool = false) {
        guard let webView = attachedWebView else { return }
        let muted = shouldMuteAudio
        let volume = min(max(AppDelegate.shared.wallpaperViewModel.effectivePlayVolume, 0), 1)
        let paused = policyPaused
        guard force || appliedPaused != paused ||
                appliedPolicyMuted != muted ||
                appliedPolicyVolume != volume else { return }
        appliedPaused = paused
        appliedPolicyMuted = muted
        appliedPolicyVolume = volume
        applyMediaPolicy(
            policyPaused: paused,
            muted: muted,
            volume: volume,
            in: webView
        )
    }
    
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        decisionHandler(.allow)
    }

    func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
        javascriptIssues.removeAll()
        mediaPolicyIssue = nil
        appliedPaused = nil
        appliedPolicyMuted = nil
        appliedPolicyVolume = nil
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        attachedWebView = webView
        // A navigation creates a new document and therefore drops all JS
        // policy markers. Re-apply both policies explicitly for the new DOM.
        appliedPaused = nil
        appliedPolicyMuted = nil
        appliedPolicyVolume = nil
        let javascriptStyle = "var css = '*{-webkit-touch-callout:none;-webkit-user-select:none}'; var head = document.head || document.getElementsByTagName('head')[0]; var style = document.createElement('style'); style.type = 'text/css'; style.appendChild(document.createTextNode(css)); head.appendChild(style);"
        evaluatePolicyJavaScript(
            javascriptStyle,
            operation: "install document input style",
            in: webView
        )
        reconcileMediaPolicy(force: true)
        applyUserProperties()
        
        if AppDelegate.shared.globalSettingsViewModel.settings.adjustMenuBarTint {
            webView.takeSnapshot(with: nil) { [weak self] nsImage, error in
                guard let self = self else { return }
                if let data = nsImage?.tiffRepresentation {
                    do {
                        let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appending(path: "staticWP_\(self.currentWallpaper.wallpaperDirectory.hashValue).tiff")
                        try data.write(to: url, options: .atomic)
                        try NSWorkspace.shared.setDesktopImageURL(url, for: .main!)
                    } catch {
                        print(error)
                    }
                }
            }
        }
    }
    
    private func applyMediaPolicy(
        policyPaused: Bool,
        muted: Bool,
        volume: Float,
        in webView: WKWebView
    ) {
        let paused = policyPaused ? "true" : "false"
        let mute = muted ? "true" : "false"
        let normalizedVolume = String(format: "%.6f", locale: Locale(identifier: "en_US_POSIX"), volume)
        evaluatePolicyAsyncJavaScript(
            "if (typeof window.__weSetMediaPolicy !== 'function') { throw new Error('media policy controller is not installed'); } await window.__weSetMediaPolicy({policyPaused:\(paused),muted:\(mute),volume:\(normalizedVolume)});",
            operation: "apply media policy",
            in: webView,
            onFailure: { [weak self] in
                self?.appliedPaused = nil
                self?.appliedPolicyMuted = nil
                self?.appliedPolicyVolume = nil
            }
        )
    }

    private func registerWebPropertyCatalog() {
        let definitions = currentWallpaper.project.general?.properties.values
            .sorted { lhs, rhs in
                switch (lhs.value.order, rhs.value.order) {
                case let (left?, right?) where left != right:
                    return left < right
                case (_?, nil):
                    return true
                case (nil, _?):
                    return false
                default:
                    let comparison = lhs.value.text.localizedStandardCompare(rhs.value.text)
                    if comparison != .orderedSame {
                        return comparison == .orderedAscending
                    }
                    return lhs.key < rhs.key
                }
            }
            .compactMap { entry in
                webPropertyDefinition(entry.key, property: entry.value)
            } ?? []
        AppDelegate.shared.wallpaperViewModel.registerScenePropertyCatalog(
            definitions,
            for: screenId,
            wallpaper: currentWallpaper
        )
    }

    private func applyUserProperties() {
        guard let webView = attachedWebView else { return }
        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        let overrides = wallpaperViewModel.scenePropertyOverrides(
            for: screenId,
            wallpaper: currentWallpaper
        )
        let entries = currentWallpaper.project.general?.properties.values ?? [:]
        var payload: [String: [String: Any]] = [:]
        for (key, property) in entries {
            guard let value = overrides[key] ?? webPropertyValue(property) else { continue }
            payload[key] = [
                "value": webPropertyJSONObject(value, property: property)
            ]
        }
        guard JSONSerialization.isValidJSONObject(payload),
              let data = try? JSONSerialization.data(withJSONObject: payload),
              let json = String(data: data, encoding: .utf8) else {
            recordJavaScriptResult(
                operation: "apply user properties",
                error: NSError(
                    domain: "WallpaperProperties",
                    code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "Web wallpaper properties contain unsupported values"]
                )
            )
            return
        }
        evaluatePolicyJavaScript(
            "if (typeof window.__weApplyUserProperties !== 'function') { throw new Error('wallpaper property bridge is not installed'); } window.__weApplyUserProperties(\(json));",
            operation: "apply user properties",
            in: webView
        )
    }

    private func evaluatePolicyJavaScript(
        _ script: String,
        operation: String,
        in webView: WKWebView
    ) {
        webView.evaluateJavaScript(script) { [weak self] _, error in
            Task { @MainActor [weak self] in
                self?.recordJavaScriptResult(operation: operation, error: error)
            }
        }
    }

    private func evaluatePolicyAsyncJavaScript(
        _ script: String,
        operation: String,
        in webView: WKWebView,
        onFailure: (() -> Void)? = nil
    ) {
        webView.callAsyncJavaScript(
            script,
            arguments: [:],
            in: nil,
            in: .page
        ) { [weak self] result in
            Task { @MainActor [weak self] in
                switch result {
                case .success:
                    self?.recordJavaScriptResult(operation: operation, error: nil)
                case .failure(let error):
                    onFailure?()
                    self?.recordJavaScriptResult(operation: operation, error: error)
                }
            }
        }
    }

    private func recordJavaScriptResult(operation: String, error: Error?) {
        if let error {
            let message = "Web wallpaper \(operation) failed: \(error.localizedDescription)"
            javascriptIssues[operation] = message
            NSLog("[Web] %@", message)
        } else {
            javascriptIssues.removeValue(forKey: operation)
        }
        mediaPolicyIssue = javascriptIssues.keys.sorted().compactMap {
            javascriptIssues[$0]
        }.joined(separator: "\n")
        if mediaPolicyIssue?.isEmpty == true {
            mediaPolicyIssue = nil
        }
    }

    /// Injected at document start so policy covers media elements created by
    /// wallpaper scripts after navigation. The controller keeps authored
    /// volume/mute and pause intent separate from host policy, allowing a
    /// host pause to restore only media that was playing before the policy
    /// became active.
    static func makeMediaPolicyUserScript() -> WKUserScript {
        WKUserScript(source: Self.mediaPolicyJavaScript, injectionTime: .atDocumentStart, forMainFrameOnly: false)
    }

    static func makePropertyListenerUserScript() -> WKUserScript {
        WKUserScript(
            source: Self.propertyListenerJavaScript,
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        )
    }

    private static let propertyListenerJavaScript = #"""
(function(){
if(window.__weWallpaperPropertyBridge){return;}
let listener=window.wallpaperPropertyListener;
let pending=null;
let hasPending=false;
const bridge={lastError:null};
function deliver(){
  if(!hasPending||!listener||typeof listener.applyUserProperties!=='function')return false;
  try{
    listener.applyUserProperties(pending);
    bridge.lastError=null;
    return true;
  }catch(error){
    bridge.lastError=String(error);
    throw error;
  }
}
const descriptor=Object.getOwnPropertyDescriptor(window,'wallpaperPropertyListener');
if(!descriptor||descriptor.configurable){
  Object.defineProperty(window,'wallpaperPropertyListener',{
    configurable:true,
    enumerable:true,
    get:function(){return listener;},
    set:function(next){
      listener=next;
      try{deliver();}catch(error){console.error('[Wallpaper Engine] applyUserProperties failed',error);}
    }
  });
}
window.__weApplyUserProperties=function(next){
  if(!next||typeof next!=='object')throw new Error('invalid wallpaper property payload');
  pending=next;
  hasPending=true;
  return deliver();
};
window.__weWallpaperPropertyBridge=bridge;
})()
"""#

    private static let mediaPolicyJavaScript = #"""
(function(){
if(window.__weMediaPolicyController){return;}
const state={policyPaused:false,muted:false,volume:1};
const controller={state:state,lastError:null};
const knownFrames=new WeakSet();
const clamp=function(v){v=Number(v);return Number.isFinite(v)?Math.max(0,Math.min(1,v)):0;};
const shouldPause=function(){return state.policyPaused;};
const snapshot=function(){return {policyPaused:state.policyPaused,muted:state.muted,volume:state.volume};};
function install(element){
  if(!element||element.__weMediaState)return;
  const authored={volume:clamp(element.volume),muted:!!element.muted,wasPlaying:false,applying:false,lastFactor:1};
  element.__weMediaState=authored;
  element.addEventListener('ended',function(){authored.wasPlaying=false;});
  element.addEventListener('pause',function(){
    if(!authored.applying&&shouldPause())authored.wasPlaying=false;
  });
  element.addEventListener('play',function(){
    if(shouldPause()&&!authored.applying){authored.wasPlaying=true;authored.applying=true;try{element.pause();}finally{authored.applying=false;}}
  });
  element.addEventListener('volumechange',function(){
    if(authored.applying)return;
    const factor=authored.lastFactor;
    if(factor>0)authored.volume=clamp(element.volume/factor);
    else authored.volume=clamp(element.volume);
    authored.muted=!!element.muted;
  });
}

function apply(element){
  install(element);
  const authored=element.__weMediaState;
  if(!authored)return Promise.resolve();
  const pause=shouldPause();
  if(pause){
    if(!element.paused)authored.wasPlaying=true;
    if(!element.paused){authored.applying=true;try{element.pause();}finally{authored.applying=false;}}
  }
  const factor=state.muted?0:state.volume;
  authored.lastFactor=factor;
  authored.applying=true;
  try{
    element.volume=clamp(authored.volume*factor);
    element.muted=state.muted||authored.muted;
  }finally{authored.applying=false;}
  if(!pause&&authored.wasPlaying){
    authored.wasPlaying=false;
    const result=element.play();
    if(result&&typeof result.catch==='function')return result.catch(function(error){controller.lastError=String(error);throw error;});
  }
  return Promise.resolve();
}
function all(){return Array.from(document.querySelectorAll('video,audio'));}
function report(error){controller.lastError=String(error);console.error('[Wallpaper Engine] media policy failed',error);}
function postFrame(frame){
  if(!frame||!frame.contentWindow)return;
  frame.contentWindow.postMessage({__weMediaPolicy:1,state:snapshot()},'*');
}
function installFrame(frame){
  if(!frame)return;
  if(!knownFrames.has(frame)){
    knownFrames.add(frame);
    frame.addEventListener('load',function(){postFrame(frame);});
  }
  postFrame(frame);
}
function framesWithin(node){
  if(node.matches&&node.matches('iframe,frame'))installFrame(node);
  if(node.querySelectorAll)node.querySelectorAll('iframe,frame').forEach(installFrame);
}
function broadcast(){document.querySelectorAll('iframe,frame').forEach(installFrame);}
const observer=new MutationObserver(function(records){
  const pending=[];
  records.forEach(function(record){record.addedNodes.forEach(function(node){
    if(node.nodeType!==1)return;
    if(node.matches&&node.matches('video,audio'))pending.push(node);
    if(node.querySelectorAll)node.querySelectorAll('video,audio').forEach(function(element){pending.push(element);});
    framesWithin(node);
  });});
  pending.forEach(function(element){apply(element).catch(report);});
});
function startObserver(){
  if(!document.documentElement)return;
  observer.observe(document.documentElement,{childList:true,subtree:true});
  broadcast();
}
if(document.documentElement)startObserver();else document.addEventListener('DOMContentLoaded',startObserver,{once:true});
controller.apply=function(){return Promise.all(all().map(apply));};
function setState(next,propagate){
  if(!next||typeof next!=='object')throw new Error('invalid media policy state');
  state.policyPaused=!!next.policyPaused;
  state.muted=!!next.muted;
  state.volume=clamp(next.volume);
  controller.lastError=null;
  if(propagate)broadcast();
  return controller.apply();
}
window.__weSetMediaPolicy=function(next){
  return setState(next,true);
};
window.addEventListener('message',function(event){
  if(window===window.top||event.source!==window.parent)return;
  const data=event.data;
  if(!data||data.__weMediaPolicy!==1)return;
  try{setState(data.state,true).catch(report);}catch(error){report(error);}
});
window.__weMediaPolicyController=controller;
setState({policyPaused:false,muted:false,volume:1},false).catch(report);
})()
"""#
}

private func webPropertyDefinition(
    _ key: String,
    property: WEProjectProperty
) -> ScenePropertyDefinition? {
    let type = property.type.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    let kind: ScenePropertyKind
    let readOnly: Bool
    switch type {
    case "bool", "boolean", "checkbox", "check":
        kind = .boolean
        readOnly = false
    case "slider", "number", "float", "integer":
        kind = .slider
        readOnly = false
    case "combo", "select", "dropdown":
        kind = .combo
        readOnly = false
    case "color", "colour":
        kind = .color
        readOnly = false
    case "textinput", "input", "string":
        kind = .textInput
        readOnly = false
    case "file":
        kind = .file
        readOnly = true
    case "directory":
        kind = .directory
        readOnly = true
    case "usershortcut", "shortcut":
        kind = .userShortcut
        readOnly = true
    case "group", "section", "text", "label", "description":
        kind = .group
        readOnly = true
    default:
        // Keep unknown project entries visible without pretending that the
        // host understands their editing semantics.
        kind = .text
        readOnly = true
    }

    let options = (property.options ?? []).map {
        ScenePropertyOption(value: $0.value.uiString, label: $0.label)
    }
    return ScenePropertyDefinition(
        key: key,
        text: property.text,
        kind: kind,
        index: property.index,
        order: scenePropertyOrder(property.order),
        minimum: property.minimum,
        maximum: property.maximum,
        step: property.step,
        precision: property.precision,
        fraction: property.fraction,
        isReadOnly: readOnly,
        options: options,
        defaultValue: webPropertyValue(property)
    )
}

private func webPropertyValue(_ property: WEProjectProperty) -> ScenePropertyValue? {
    let type = property.type.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    switch type {
    case "bool", "boolean", "checkbox", "check":
        return property.value?.booleanSceneValue
    case "slider", "number", "float":
        return property.value?.numericSceneValue(integerOnly: false)
    case "integer":
        return property.value?.numericSceneValue(integerOnly: true)
    case "combo", "select", "dropdown":
        guard let value = property.value else { return nil }
        return .string(value.uiString)
    case "textinput", "input", "string", "file", "directory", "usershortcut", "shortcut":
        return .string(property.value?.uiString ?? "")
    case "group", "section", "text", "label", "description":
        return nil
    default:
        return property.value.map { .string($0.uiString) }
    }
}

private func webPropertyJSONObject(
    _ value: ScenePropertyValue,
    property: WEProjectProperty
) -> Any {
    let type = property.type.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    if ["combo", "select", "dropdown"].contains(type),
       case .string(let selected) = value {
        if let option = property.options?.first(where: { $0.value.uiString == selected }) {
            return option.value.jsonObject
        }
        if let defaultValue = property.value,
           defaultValue.uiString == selected {
            return defaultValue.jsonObject
        }
    }
    switch value {
    case .boolean(let value): return value
    case .integer(let value): return value
    case .number(let value): return value
    case .string(let value): return value
    }
}

private func scenePropertyOrder(_ value: Double?) -> Int? {
    guard let value, value.isFinite else { return nil }
    return Int(exactly: value)
}

private extension WEProjectPropertyValue {
    var uiString: String {
        switch self {
        case .boolean(let value): return value ? "true" : "false"
        case .integer(let value): return String(value)
        case .number(let value): return String(value)
        case .string(let value): return value
        }
    }

    var jsonObject: Any {
        switch self {
        case .boolean(let value): return value
        case .integer(let value): return value
        case .number(let value): return value
        case .string(let value): return value
        }
    }

    var booleanSceneValue: ScenePropertyValue? {
        switch self {
        case .boolean(let value):
            return .boolean(value)
        case .integer(let value) where value == 0 || value == 1:
            return .boolean(value == 1)
        case .number(let value) where value == 0 || value == 1:
            return .boolean(value == 1)
        case .string(let value):
            switch value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
            case "1", "true", "yes", "on": return .boolean(true)
            case "0", "false", "no", "off": return .boolean(false)
            default: return nil
            }
        default:
            return nil
        }
    }

    func numericSceneValue(integerOnly: Bool) -> ScenePropertyValue? {
        switch self {
        case .integer(let value):
            return integerOnly ? .integer(value) : .number(Double(value))
        case .number(let value):
            guard value.isFinite else { return nil }
            if integerOnly {
                guard let integer = Int64(exactly: value) else { return nil }
                return .integer(integer)
            }
            return .number(value)
        case .string(let value):
            if integerOnly, let integer = Int64(value.trimmingCharacters(in: .whitespacesAndNewlines)) {
                return .integer(integer)
            }
            guard !integerOnly, let number = Double(value), number.isFinite else { return nil }
            return .number(number)
        case .boolean:
            return nil
        }
    }
}
