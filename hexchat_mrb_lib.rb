# rubocop:disable Metrics/LineLength
# rubocop:disable Metrics/MethodLength
# rubocop:disable Metrics/ClassLength
# rubocop:disable Style/ClassAndModuleChildren
# rubocop:disable Metrics/BlockNesting
# Meh, it's complicated.
# rubocop:disable Metrics/AbcSize
# rubocop:disable Metrics/CyclomaticComplexity
# rubocop:disable Metrics/PerceivedComplexity
# See README.md
# rubocop:disable Style/Documentation
# Global vars needed for unhook trick.
# rubocop:disable Style/GlobalVars
# MRuby doesn't support freezing constants.
# rubocop:disable Style/MutableConstant

module HexChat
  VERSION = '0.0.1'

  COLORS = {
    white: 0,
    black: 1,
    blue: 2,
    navy: 2,
    green: 3,
    light_red: 4,
    brown: 5,
    maroon: 5,
    purple: 6,
    orange: 7,
    olive: 7,
    yellow: 8,
    light_green: 9,
    lime: 9,
    cyan: 10,
    teal: 10,
    light_cyan: 11,
    aqua: 11,
    light_blue: 12,
    royal: 12,
    pink: 13,
    light_purple: 13,
    fuchsia: 13,
    grey: 14,
    light_grey: 15,
    silver: 15,
    default: 99
  }

  COLOR = "\x03"
  BOLD = "\x02"
  ITALIC = "\x1D"
  UNDERLINE = "\x1F"
  INVERSE = "\x16"
  RESET = "\x0F"

  CHAN_CONNECTED = 1
  CHAN_CONNECTING = 2
  CHAN_AWAY = 3
  CHAN_END_OF_MOTD = 8
  CHAN_WHOX = 16
  CHAN_IDMSG = 32
  CHAN_HIDE_JOIN_PART = 65
  CHAN_HIDE_JOIN_PART_UNSET = 128
  CHAN_BEEP = 256
  CHAN_BLINK_TRAY = 512
  CHAN_BLINK_TASKBAR = 1024
  CHAN_LOGGING = 2048
  CHAN_LOGGING_UNSET = 4096
  CHAN_SCROLLBACK = 8192
  CHAN_SCROLLBACK_UNSET = 16_384
  CHAN_STRIP = 32_768
  CHAN_STRIP_UNSET = 65_536
  TYPE_SERVER = 1
  TYPE_CHANNEL = 2
  TYPE_DIALOG = 3
  TYPE_NOTICE = 4
  TYPE_SNOTICE = 5

  DCC_QUEUED = 0
  DCC_ACTIVE = 1
  DCC_FAILED = 2
  DCC_DONE = 3
  DCC_CONNECTING = 4
  DCC_ABORTED = 5
  TYPE_SEND = 0
  TYPE_RECV = 1
  TYPE_CHAT_RECV = 2
  TYPE_CHAT_SEND = 3

  IGNORE_PRIVATE = 0
  IGNORE_NOTICE = 1
  IGNORE_CHANNEL = 2
  IGNORE_CTCP = 3
  IGNORE_INVITE = 4
  IGNORE_UNIGNORE = 5
  IGNORE_NOSAVE = 6
  IGNORE_DCC = 7

  # Convenience method to get color codes
  def self.color(fore = nil, back = nil)
    return COLOR if !fore || fore == :none
    f = fore.is_a?(Symbol) ? COLORS[fore] : fore
    b = back.is_a?(Symbol) ? COLORS[back] : back
    t = b ? ",#{b}" : ''
    "#{COLOR}#{f}#{t}"
  end

  # Internal functions that should not be called by the user
  # The C code also defines methods here
  class Internal
    class << self
      # Called by C for /mrb command
      def mrb_command(word, _word_eol)
        command = word[1]
        arg = word[2]
        # args = word_eol[2]
        case command.downcase
        when 'help'
          print('  /MRB - open MRuby Console')
          print('  /MRB EVAL <ruby code> - evaluate the given code')
          print('  /MRB LOAD <file> - Load the given file')
          print('  /MRB UNLOAD <class> - Unregister the given plugin class')
          print('  /MRB LIST - List plugin classes')
        when 'load'
          HexChat::Plugin::Registry.load(arg) if arg
        when 'unload'
          HexChat::Plugin::Registry.unload(arg) if arg
        when 'list'
          HexChat::Plugin::Registry.list
        else
          print("Unknown command: #{command}")
        end
      end

      # Called by C when plugin deinits
      def cleanup
        (h, u) = free_lists
        puts "cleanup: Freed #{h}/#{h + u} lists"
        (h, u) = unhook_all
        puts "cleanup: Unhooked #{h}/#{h + u} hooks"
      end

      private

      # Free all open lists
      def free_lists
        u_count = 0
        h_count = 0
        ObjectSpace.each_object do |o|
          next unless o.is_a?(HexChat::Internal::List)
          if o.free?
            u_count += 1
          else
            o.free
            h_count += 1
          end
        end
        [h_count, u_count]
      end

      # Unhook all hooks
      def unhook_all
        u_count = 0
        h_count = 0
        ObjectSpace.each_object do |o|
          next unless o.is_a?(HexChat::Internal::Hook)
          if o.hooked?
            o.unhook
            h_count += 1
          else
            u_count += 1
          end
        end
        [h_count, u_count]
      end
    end
  end

  # Wraps a HexChat::Internal::Context into something useful
  class Context
    class << self
      # Retrieve the current context
      def current
        new(HexChat::Internal::Context.current)
      end

      # Retrieve the focused context
      def focused
        find(nil, nil)
      end

      # Retrieve an arbitrary context
      def find(server = nil, channel = nil)
        c = HexChat::Internal::Context.find(server, channel)
        c ? new(c) : nil
      end
    end

    attr_reader :context

    def initialize(c)
      fail 'do not instantiate HexChat::Context' unless c.is_a?(HexChat::Internal::Context)
      @context = c
    end

    # Make this context active
    def set
      @context.set
    end

    # Compare this context to another context
    def ==(other)
      other.is_a?(HexChat::Context) ? @context.ptr == other.context.ptr : false
    end

    # Temporarily set this context and execute the block
    def with(&_block)
      c = HexChat::Internal::Context.current
      set
      result = yield
      c.set
      result
    end
  end

  # Base class for HexChat lists plus dynamic generator functions
  class List
    class << self
      # Return fields for the current list
      def fields
        return nil if self == HexChat::List
        @fields
      end

      # Return list name
      def name
        return nil if self == HexChat::List
        @name
      end

      # Set values for the list, used during construction of the list class
      def set_values(fields, name)
        return nil if self == HexChat::List
        @fields = fields
        @name = name
      end

      # Create all the list classes dynamically
      def create_list_classes
        return nil unless self == HexChat::List
        lists = HexChat::Internal::List.fields('lists')
        lists.each { |l| create_list_class(l) } if lists
      end

      # Create an individual list class dynamically
      def create_list_class(list)
        return nil unless self == HexChat::List
        klass_name = list.capitalize
        klass = Class.new(HexChat::List)
        HexChat::List.const_set(klass_name, klass)
        fields = HexChat::Internal::List.fields(list)
        field_hash = {}
        fields.each do |f|
          t = f[0]
          n = f[1..-1].to_sym
          type = case t
                 when 's'
                   :string
                 when 'i'
                   :integer
                 when 't'
                   :time
                 when 'p'
                   :context if n == :context
                 end
          field_hash[n] = { type: type, name: f }
        end
        klass.set_values(field_hash, list)
      end

      # Call block for each list item
      def each
        fail 'do not call #each on HexChat::List' if self == HexChat::List
        if block_given?
          list = HexChat::Internal::List.get(name)
          # puts list.inspect
          while list.next
            h = {}
            fields.each_pair do |n, i|
              # puts "#{n} #{i[:type]} #{i}"
              h[n] = case i[:type]
                     when :string
                       list.str(n.to_s)
                     when :time
                       Object.const_defined?('Time') ? Time.at(list.time(n.to_s)) : list.time(n.to_s)
                     when :integer
                       list.int(n.to_s)
                     when :context
                       HexChat::Context.new(list.cxt(n.to_s))
                     end
            end
            yield h
          end
          list.free
        end
        nil
      end

      # Return all list items as an array
      def all
        fail 'do not call #all on HexChat::List' if self == HexChat::List
        a = []
        each { |h| a.push(h) }
        a
      end

      alias to_a all
    end

    def initialize
      fail "do not instantiate a #{self.class.name} list"
    end
  end

  # Wrap a hook into something usable.  User should normally not
  # use this and instead use the methods provided by the HexChat::Plugin
  # class
  class Hook
    attr_reader :name, :fd, :timeout

    # Set up a new hook.  Inst will be the object that the hook
    # block will be instance_evaluated
    def initialize(inst)
      @inst = inst
      @hook = HexChat::Internal::Hook.new { |*args| call(*args) }
      @hook.set_ref(self)
    end

    # Set the hook for an action
    def on(type, name, opts = {}, &block)
      unhook if hooked?
      @block = block
      priority = opts[:priority] || HexChat::PRI_NORM
      case type
      when :command
        fail 'command name must be a String' unless name.is_a?(String)
        @name = name
        @hook.hook_command(name, opts[:help] || '', priority)
      when :fd
        fail 'fd must be an Integer' unless name.is_a?(Integer)
        @fd = name
      when :print
        fail 'print event name must be a String' unless name.is_a?(String)
        @name = name
        @hook.hook_print(name, priority)
      when :server
        fail 'server event name must be a String' unless name.is_a?(String)
        @name = name
        @hook.hook_server(name, priority)
      when :timer
        fail 'timeout must be an Fixnum' unless name.is_a?(Fixnum)
        @timeout = name
        @hook.hook_timer((name * 1000).to_i)
      else
        fail "event type #{type} unknown"
      end
      self
    end

    # Call the hook's block
    def call(*args)
      fail 'block not set' unless @block.is_a?(Proc)
      @inst.instance_exec(*args, &@block)
    end

    def hooked?
      @hook.hooked?
    end

    def unhook
      @hook.unhook
    end
  end

  # The base class for all plugins.  Users should subclass this to create
  # an MRuby plugin.  The plugin class definition must call register at the
  # end in order to register the plugin.
  class Plugin
    # Bring in the constants
    include HexChat

    # Class methods are used to set up hooks and setup/cleanup code, these will be
    # deferred until the class is instantiated.
    # It is also provides for registering the plugin, which creates an instance.
    class << self
      attr_accessor :deferred_hooks

      # Define a setup block
      def setup(&block)
        HexChat::Plugin::Registry.defer_hook(self, type: :setup, name: nil, opts: {}, block: block)
        nil
      end

      # Define a cleanup block
      def cleanup(&block)
        HexChat::Plugin::Registry.defer_hook(self, type: :cleanup, name: nil, opts: {}, block: block)
        nil
      end

      # Define a hook
      def on(type, name, opts = {}, &block)
        HexChat::Plugin::Registry.defer_hook(self, type: type, name: name, opts: opts, block: block)
        nil
      end

      # Register this plugin
      def register
        HexChat::Plugin::Registry.register(self)
      end

      # I wish this worked, wonder if there's a way to delay response
      # until the class finishes being initialized
      # def inherited(klass)
      #  puts "Inherited #{klass} in #{self}"
      #  return nil unless self == HexChat::Plugin
      #  HexChat::Plugin::Registry.register(klass)
      # end

      # Unregiter this plugin
      def unregister
        HexChat::Plugin::Registry.unregister(self)
      end
    end

    attr_reader :hooks

    def initialize
      @hooks = []
      @cleanup = []
      setup_deferred
      self
    end

    def cleanup
      @hooks.each(&:unhook)
      @cleanup.each do |block|
        instance_eval(&block)
      end
      @cleanup.clear
    end

    private

    def setup_deferred
      HexChat::Plugin::Registry.deferred_hooks(self.class).each do |r|
        case r[:type]
        when :setup
          instance_eval(&r[:block])
        when :cleanup
          @cleanup.push(r[:block])
        else
          on r[:type], r[:name], r[:opts] { |*args| instance_exec(*args, &r[:block]) }
        end
      end
      HexChat::Plugin::Registry.deferred_hooks(self.class).clear
    end

    def color(f = nil, b = nil)
      HexChat.color(f, b)
    end

    def print(*args)
      args.each do |a|
        out = a.to_s || "<to_s on #{a.class}>"
        HexChat::Internal.print(out)
      end
    end

    alias puts print

    def emit_print(*args)
      HexChat::Internal.emit_print(*args)
    end

    def nickcmp(n1, n2)
      HexChat::Internal.nickcmp(n1, n2)
    end

    def command(*args)
      args.each do |a|
        HexChat::Internal.command(a)
      end
    end

    def get_info(item)
      HexChat::Internal.get_info(item)
    end

    def get_prefs(*prefs)
      if prefs.is_a?(Array) && prefs.count > 1
        prefs.map { |p| HexChat::Internal.get_prefs(p) }
      elsif prefs.is_a?(Array)
        HexChat::Internal.get_prefs(prefs.first)
      else
        HexChat::Internal.get_prefs(prefs)
      end
    end

    def pluginpref_set_str(var, value)
      HexChat::Internal.pluginpref_set_str(var.to_s, value.to_s)
    end

    def pluginpref_get_str(var)
      HexChat::Internal.pluginpref_get_str(var.to_s)
    end

    def pluginpref_set_int(var, value)
      HexChat::Internal.pluginpref_set_int(var.to_s, value.to_i)
    end

    def pluginpref_get_int(var)
      HexChat::Internal.pluginpref_get_intr(var.to_s)
    end

    def away
      get_info('away')
    end

    def away=(s)
      command("away #{s}")
    end

    def network
      get_info('network')
    end

    def server
      get_info('server')
    end

    def channel
      get_info('channel')
    end

    def topic
      get_info('topic')
    end

    def topic=(s)
      command("topic #{s}")
    end

    def strip(s, flags = HexChat::STRIP_ALL)
      HexChat::Internal.strip(s, flags)
    end

    def on(type, name, opts = {}, &block)
      hook = HexChat::Hook.new(self)
      @hooks.push(hook)
      hook.on(type, name, opts, &block)
    end

    def unhook
      fail 'not unhookable' unless $__HOOK_REF != self && $__HOOK_REF.respond_to?(:unhook)
      $__HOOK_REF.unhook
    end

    def on_old(type, name, opts = {}, &block)
      hook = HexChat::Internal::Hook.new(&block)
      @hooks.push(hook)
      case type
      when :command
        fail 'command name must be a String' unless name.is_a?(String)
        hook.hook_command(name, opts[:help] || '')
      when :fd
        fail 'fd must be an Integer' unless name.is_a?(Integer)
        fail 'flags option (Integer) is required' unless opts[:flags].is_a?(Integer)
        hook.hook_fd(fd, flags)
      when :print
        fail 'print event name must be a String' unless name.is_a?(String)
        hook.hook_print(name)
      when :server
        fail 'server event name must be a String' unless name.is_a?(String)
        hook.hook_server(name)
      when :timer
        fail 'timeout must be an Fixnum' unless name.is_a?(Fixnum)
        hook.hook_timer((name * 1000).to_i)
      else
        fail "event type #{type} unknown"
      end
      hook
    end

    def unregister
      self.class.unregister
    end
  end

  class Plugin::Registry
    class << self
      attr_reader :registry

      def autoload
        if Object.const_defined?(:Dir)
          dir = "#{HexChat::Internal.get_info('configdir')}/mruby"
          if Dir.exist?(dir)
            files = Dir.new(dir).enum_for.select { |f| f.end_with?('.rb', '.mrb') }
            files.each { |f| load(f) }
          else
            HexChat::Internal.print("MRuby: #{dir} not found, autoloading disabled")
          end
        else
          HexChat::Internal.print('MRuby: "Dir" missing, autoloading disabled')
        end
        nil
      end

      def load(plugin)
        file = plugin
        if Object.const_defined?(:File)
          file = [
            file,
            "#{HexChat::Internal.get_info('configdir')}/mruby/#{file}",
            "#{HexChat::Internal.get_info('libdirfs')}/mruby/#{file}"
          ].detect { |f| File.exist?(f) }
        end
        success = HexChat::Internal.load(file) if file
        if success
          HexChat::Internal.print("MRuby: Loaded #{plugin}")
        else
          HexChat::Internal.print("MRuby: Unable to load #{plugin}: #{success.nil? ? 'not found' : 'error'}")
        end
        success
      end

      def unload(klass_name)
        klass = nil
        if klass_name
          ObjectSpace.each_object do |o|
            klass = o if o.is_a?(Class) && o.ancestors.include?(HexChat::Plugin) && o.to_s == klass_name
          end
        end
        if klass && klass != HexChat::Plugin
          klass.unregister
        else
          print("Cannot unload: #{klass_name.inspect}")
        end
      end

      def list
        if @registry
          puts "MRuby registered plugins: #{@registry.keys.map(&:to_s).join(', ')}"
        else
          puts 'MRuby: no registered plugins'
        end
      end

      def registered?(klass)
        (@registry ||= {}).key?(klass)
      end

      def register(klass)
        fail "#{klass} is already registered" if registered?(klass)
        (@registry ||= {})[klass] = klass.new
        HexChat::Internal.print("Registered MRuby plugin #{klass}")
        klass
      end

      def unregister(klass)
        fail "#{klass} is not registered" unless registered?(klass)
        @registry[klass].cleanup
        HexChat::Internal.print("Unregistered MRuby plugin #{klass}")
        (@registry ||= {}).delete(klass)
      end

      def defer_hook(klass, hook_def)
        ((@deferred_hooks ||= {})[klass] ||= []).push(hook_def)
      end

      def deferred_hooks(klass)
        (@deferred_hooks ||= {})[klass] || []
      end
    end
  end
end # module HexChat

# I/O

def print(*args)
  args.each do |a|
    out = a.to_s || "<to_s on #{a.class}>"
    HexChat::Internal.print(out)
  end
  nil
end

def puts(*args)
  print(*args)
end

HexChat::List.create_list_classes

# Little shorthand/compatibility
XChat = HexChat

HexChat::Plugin::Registry.autoload

puts "MRuby interface initialized, version #{HexChat::VERSION}"
