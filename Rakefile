PLUGIN_ROOT = File.dirname(File.expand_path(__FILE__))
MRUBY_HOME  = "#{PLUGIN_ROOT}/mruby"
MRUBY_CONFIG = "#{PLUGIN_ROOT}/build_config.rb"

ENV['MRUBY_CONFIG'] = MRUBY_CONFIG

task :default => [:build]

desc "Build MRuby"
task :mruby_build do
  File.exist?("#{MRUBY_HOME}/Rakefile") || abort("Put MRuby source into the mruby directory!")
  cd = Dir.pwd
  Dir.chdir(MRUBY_HOME)
  sh 'ruby ./minirake'
  Dir.chdir(cd)
end

desc "Compile Ruby code to .h"
task :hexchat_mrb_lib => [:mruby_build] do
  sh './mruby/bin/mrbc -g -Bhexchat_mrb_lib -ohexchat_mrb_lib.h hexchat_mrb_lib.rb'
end

desc "Build the plugin"
task :build => [:mruby_build, :hexchat_mrb_lib] do
  sh 'gcc mruby.c -O0 -ggdb -Wall -shared -fPIC -o mruby.so -Imruby/include mruby/build/host/lib/libmruby.a'
end

desc "Clean MRuby"
task :mruby_clean do
  cd = Dir.pwd
  Dir.chdir(MRUBY_HOME)
  sh 'ruby ./minirake clean'
  Dir.chdir(cd)
end

desc "Clean the plugin"
task :plugin_clean do
  sh 'rm -f mruby.so'
  sh 'rm -f hexchat_mrb_lib.h'
end

desc "Clean all"
task :clean => [:plugin_clean, :mruby_clean] do
  puts "Cleaned!"
end

desc "Install in ~/.config/hexchat/..."
task :install => [:build] do
  sh '[ -d ~/.config/hexchat/addons ]'
  sh 'cp -f mruby.so ~/.config/hexchat/addons'
  sh 'mkdir -p ~/.config/hexchat/mruby'
  puts "Installed!"
end
