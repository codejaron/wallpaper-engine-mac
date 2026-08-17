#!/usr/bin/env ruby
# frozen_string_literal: true

require "json"
require "optparse"
require "pathname"
require "set"

options = {
  abi: Pathname(__dir__).join(
    "../Sources/SceneMetal/OfficialShaderABI.hpp"
  ).cleanpath,
  renderer: Pathname(__dir__).join(
    "../Sources/SceneMetal/FramePlanExecutor.cpp"
  ).cleanpath,
  strict: false,
}
OptionParser.new do |parser|
  parser.banner = "Usage: audit_wallpaper_engine_shader_abi.rb --assets PATH [options]"
  parser.on(
    "--assets PATH",
    "Directory containing loose shaders and/or scene.pkg files"
  ) do |value|
    options[:assets] = Pathname(value)
  end
  parser.on("--abi PATH", "Versioned executable ABI header") do |value|
    options[:abi] = Pathname(value)
  end
  parser.on("--renderer PATH", "Renderer implementation to inspect") do |value|
    options[:renderer] = Pathname(value)
  end
  parser.on("--strict", "Fail when any official ABI builtin has no provider") do
    options[:strict] = true
  end
end.parse!

abort("--assets is required") unless options[:assets]
abort("assets directory does not exist: #{options[:assets]}") unless options[:assets].directory?

abi_source = options[:abi].read
uniform_names = abi_source.scan(/std::string_view\{"(g_[A-Za-z0-9_]+)"\}/).flatten
abort("expected 140 official ABI entries, found #{uniform_names.length}") unless uniform_names.length == 140
abort("official ABI contains duplicate names") unless uniform_names.uniq.length == uniform_names.length

official_ids = uniform_names.each_with_index.to_h
observed = Hash.new { |hash, name| hash[name] = Hash.new(0) }
observed_sources = Hash.new { |hash, name| hash[name] = Set.new }
shader_extensions = %w[frag vert geom comp glsl inc]

def read_package_u32(bytes, cursor, source)
  raise "#{source}: truncated uint32 at #{cursor}" if cursor + 4 > bytes.bytesize

  [bytes.byteslice(cursor, 4).unpack1("V"), cursor + 4]
end

def read_package_string(bytes, cursor, source, maximum_length)
  length, cursor = read_package_u32(bytes, cursor, source)
  if length > maximum_length
    raise "#{source}: string length #{length} exceeds #{maximum_length}"
  end
  if cursor + length > bytes.bytesize
    raise "#{source}: truncated string at #{cursor}"
  end

  [bytes.byteslice(cursor, length), cursor + length]
end

def package_shader_sources(path, shader_extensions)
  bytes = path.binread
  cursor = 0
  version, cursor = read_package_string(bytes, cursor, path, 64)
  raise "#{path}: expected PKGV package header" unless version.start_with?("PKGV")

  entry_count, cursor = read_package_u32(bytes, cursor, path)
  raise "#{path}: entry count #{entry_count} exceeds 100000" if entry_count > 100_000

  entries = Array.new(entry_count) do
    logical_path, cursor = read_package_string(bytes, cursor, path, 16 * 1024)
    offset, cursor = read_package_u32(bytes, cursor, path)
    length, cursor = read_package_u32(bytes, cursor, path)
    [logical_path.force_encoding(Encoding::UTF_8).scrub, offset, length]
  end
  payload_size = bytes.bytesize - cursor

  entries.each_with_object([]) do |(logical_path, offset, length), sources|
    extension = File.extname(logical_path).delete_prefix(".").downcase
    next unless shader_extensions.include?(extension)
    if offset > payload_size || length > payload_size - offset
      raise "#{path}: package entry #{logical_path.inspect} points outside payload"
    end

    sources << [
      "#{path}:#{logical_path}",
      bytes.byteslice(cursor + offset, length),
    ]
  end
end

shader_sources = shader_extensions.flat_map do |extension|
  options[:assets].glob("**/*.#{extension}").map do |path|
    [path.to_s, path.binread]
  end
end
options[:assets].glob("**/scene.pkg").each do |path|
  begin
    shader_sources.concat(package_shader_sources(path, shader_extensions))
  rescue StandardError => error
    abort("failed to parse #{path}: #{error.message}")
  end
end

uniform_pattern = /\buniform\s+(?:lowp\s+|mediump\s+|highp\s+)?([A-Za-z0-9_]+)\s+(g_[A-Za-z0-9_]+)(\s*\[[^\]]+\])?\s*;/
shader_sources.each do |source, bytes|
  bytes.force_encoding(Encoding::UTF_8).scrub.scan(uniform_pattern) do |type, name, array|
    next unless official_ids.key?(name)

    declaration = type + (array ? array.gsub(/\s+/, "") : "")
    observed[name][declaration] += 1
    observed_sources[name] << source
  end
end

renderer_source = options[:renderer].read
# A string literal is not a provider. Count only builtins that enter the
# executor's typed uniform-preparation path; binding/value correctness is then
# covered by the renderer's pixel tests. This prevents diagnostics, comments,
# or a name-only compatibility table from satisfying the audit.
provider_pattern = /prepare[A-Za-z0-9_]*Uniform\(\s*(?:program|programResource),\s*"(g_[A-Za-z0-9_]+)"/
providers = renderer_source.scan(provider_pattern).flatten.to_set

# These families are prepared from the reflected slot number rather than
# spelled out once per slot in production code.
(0..9).each do |slot|
  providers << "g_Texture#{slot}"
  providers << "g_Texture#{slot}Resolution"
  providers << "g_Texture#{slot}Texel"
  providers << "g_Texture#{slot}MipMapInfo"
  providers << "g_Texture#{slot}Rotation"
  providers << "g_Texture#{slot}Translation"
end

entries = uniform_names.each_with_index.map do |name, id|
  declarations = observed[name]
    .sort_by { |type, count| [-count, type] }
    .to_h
  {
    id: id,
    name: name,
    observed_declarations: declarations,
    observed_sources: observed_sources[name].sort,
    declared_by_assets: !declarations.empty?,
    provider_prepared_by_renderer: providers.include?(name),
  }
end

missing_asset_providers = entries.select do |entry|
  entry[:declared_by_assets] &&
    !entry[:provider_prepared_by_renderer]
end
missing_registry_providers = entries.reject do |entry|
  entry[:provider_prepared_by_renderer]
end

report = {
  wallpaper_engine_version: "2.8.0.42",
  registry_entry_count: entries.length,
  scanned_shader_source_count: shader_sources.length,
  declared_by_assets_count: entries.count { |entry| entry[:declared_by_assets] },
  provider_prepared_by_renderer_count: entries.count { |entry| entry[:provider_prepared_by_renderer] },
  missing_registry_provider_count: missing_registry_providers.length,
  missing_registry_providers: missing_registry_providers.map do |entry|
    entry.slice(:id, :name, :observed_declarations, :observed_sources)
  end,
  missing_asset_provider_count: missing_asset_providers.length,
  missing_asset_providers: missing_asset_providers.map do |entry|
    entry.slice(:id, :name, :observed_declarations, :observed_sources)
  end,
  uniforms: entries,
}

puts JSON.pretty_generate(report)
exit(2) if options[:strict] && !missing_registry_providers.empty?
