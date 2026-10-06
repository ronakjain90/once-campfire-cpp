# Writes erb/<case>.<set>.expected: the output of the ERB file compiled by Erubi (the engine of
# Rails ActionView, trim mode on). The C++ test
# compares the output of the matching .ct file with it. Run it in the campfire-reference image:
#   docker run --rm -v $PWD/src/views/test:/t campfire-reference:app bundle exec ruby /t/gen_expected.rb /t
require "erb"
require "erubi"

DIR = ARGV.fetch(0)
SETS = {
  "a" => { name: "<b>&\"' é", markup: "<i>safe</i>", items: %w[a b c], n: 2, flag: true, nothing: nil },
  "b" => { name: "", markup: "", items: [], n: 7, flag: false, nothing: nil },
}

class Ctx
  def initialize(vars) = vars.each { |k, v| instance_variable_set("@#{k}", v) ; define_singleton_method(k) { v } }
  def render_item = ->(label, index) { "<li data-i=\"#{index}\">#{ERB::Util.h(label)}</li>\n" }
  def get_binding = binding
end

Dir[File.join(DIR, "erb", "*.erb")].sort.each do |path|
  base = File.basename(path, ".erb")
  SETS.each do |set, vars|
    out = eval(Erubi::Engine.new(File.binread(path)).src, Ctx.new(vars).get_binding)
    File.binwrite(File.join(DIR, "erb", "#{base}.#{set}.expected"), out)
  end
end
