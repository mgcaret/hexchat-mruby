# HexChat MRuby Plugin

This is a plugin for the HexChat IRC client that embeds [MRuby](http://mruby.org) into HexChat.

## Building & Installing

### Unix

#### Building

1. Install Ruby and the dev tools for your distro.
2. Download MRuby (tested with 1.2.0) and extract it into the `./mruby` directory of the plugin source.
3. Edit `build_config.rb` to add/remove mrbgems and other options for MRuby.   Leaving in mruby-io and mruby-dir (or another gem that provides the File and Dir) is strongly recommended or script autoloading won't work.
4. Run this command: `./build.sh` (this uses the minirake provided in the MRuby source).

### Installation

#### Manually

Place `mruby.so` into `$HOME/.config/hexchat/addons`

Create a directory `$HOME/.config/hexchat/mruby` to hold MRuby scripts for auto-loading.

#### Automatically

Run `./build.sh install`

### Windows

If you can make it build in Windows, send me a pull request.

## Usage

Placing scripts in `$HOME/.config/hexchat/mruby` will cause them to be automatically loaded when the MRuby plugin is loaded *if* the Dir class is available. 

### Commands                                                                                                       

`/mrb help` - Provide help for `/mrb` command.

`/mrb` - open MRuby console

This will open a query window that is intercepted by the plugin and functions as an MRuby REPL.  This does not allow continuations of unfinished code as irb/mirb/pry do, each line must stand fully on its own.                      

`puts` and `print` will output to the HexChat window.

`/mrb eval <ruby code>` - Eval given code.

This will eval the given MRuby code and print the result. 

`/mrb load <script>` - Load an MRuby script

If the File class is available, this will search for the script first under the given name, otherwise under the HexChat config dir and plugin directories in the mruby subdirectory.  If the File class is not available, the given name will be tried.

This command is able to load Ruby source code, as well as code compiled by `mrbc` (RiteVM code).

`/mrb unload <plugin class>` - Unload an MRuby plugin

This will "unload" a registered MRuby plugin.  What this really means is the plugin's cleanup code is called, all hooks belonging to it are unhooked, and the instance is unregistered.  The plugin can then be loaded again.

## Writing HexChat MRuby Plugins

It is strongly recommended at this point that you read the HexChat C [plugin documentation](http://hexchat.readthedocs.org/en/latest/plugins.html) before proceeding to understand what you are expected to provide and what will be provided to you.

The plugin class provides several helper methods, see the section below for the list of them.

All HexChat MRuby plugins extend `HexChat::Plugin` and the class definition must end with the `register` call before the class is closed.

All blocks defined for hooks are evaluated in the plugin instance.

### Examples

See the `examples` directory.

### DSL Style Plugin Class Layout

The usual style of plugin uses a DSL where you will define zero or more *setup* blocks, zero or more *hooks* and zero or more *cleanup* blocks.

These definitions are deferred until an instance of the class is created with `register` and then they are created.

### "Traditional" Style Plugin Class Layout

Alternatively, you can define an `initialize` method (be sure to call `super`) and do things from there.  In this case, setup blocks are immediately executed, other hooks behave as described below.

### Setup Blocks

Setup blocks are used to initialize the plugin.  They take the following form:

```ruby
setup do
  # put your setup code here
end
```

Each setup block is executed in the order it was defined.

### Cleanup Blocks

Cleanup blocks are called when the plugin is being unregistered.  They take the following form:

```ruby
cleanup do
  # put your cleanup code here
end
```

Each cleanup block is executed in the order it was defined.

### Hooks

Hooks are the bits that do the actual work.  A hook registers interest in a HexChat event and provides code to execute when that event occurs.

All hooks take an option hash.  Command, Server, and Print hooks accept a `:priority` key with one of the following values:  `HexChat::PRI_HIGHEST`, `HexChat::PRI_HIGH`, `HexChat::PRI_NORM` (default), `HexChat::PRI_LOW`, `HexChat::PRI_LOWEST`.

#### Hook Return Values

Hooks must return an integer value to tell HexChat what action to take after you have processed the hook.  The values are provided as constants in the HexChat module and are as follows:

Value                | Action
---------------------|--------
HexChat::EAT_NONE    | Continue processing.
HexChat::EAT_ALL     | No further processing.
HexChat::EAT_HEXCHAT | Other plugins can process this, but HexChat will not.
HexChat::EAT_PLUGIN  | Other plugins cannot process this, but Hexchat will.  

For your convenience these constants are imported into the `HexChat::Plugin` class.

#### Command Hooks

`on :command, <cmd_name>, opts = {} {|word, word_eol|}` 

This registers interest in a command such as `/foo` typed into the HexChat input box.  The block is called with two arrays as parameters corresponding to the *word* and *word_eol* arrays provided by HexChat to plugins.

One option is defined, `:help` which defines the help text for the command.

The hook must return a value that defines what HexChat should do with the command after you have processed it. 

```ruby
on :command, 'foo', :help => 'The foo command!' do |word, word_eol|
  print "/FOO was called!"
  EAT_ALL
end
```

#### Print Event Hooks

`on :print, <event_name>, opts = {} {|word|}` 

This registers interest in a HexChat print event.  Print events are generated whenever HexChat is going to write to an output window.  See the HexChat [plugin interface documentation](http://hexchat.readthedocs.org/en/latest/plugins.html#hook-functions) for what events can be specified and what `word` will contain.

```ruby
on :print, 'Channel Message' do |word|
  # do something with channel message
  EAT_NONE
end
```

#### Server Hooks

`on :server, <message_id>, opts = {} {|word, word_eol|}` 

This registers interest in an IRC server message event.  The server message may be any IRC message type, including numerics.

```ruby
on :server, "KICK" do |word, word_eol|
  puts "#{word[3] was kicked from #{word[2]} (reason: {word_eol[4]})"
  EAT_NONE
end
```

#### Timer Hooks

`on :timer, <seconds> { }` 

This registers a periodic timer that will be called.  

```ruby
on :timer, 60 do
  puts "60-second timer!"
  EAT_NONE
end
```

To do a one-shot timer:

```ruby
on :timer, 10 do
  puts "10 seconds elapsed!"
  unhook
  EAT_NONE
end
```

#### FD Hooks

`on :fd, <fileno>, flags: <flags> { }` 

To use these you must have an IO implementation compiled into MRuby that can return the underlying file descriptor.

Typically this would be used after some event or setup has occurred and not be defined at the top level of the plugin.

The flags option must be specified and can be one of the following: `HexChat::FD_READ`, `HexChat::FD_WRITE`, `HexChat::FD_EXCEPTION`, or `HexChat::FD_NOTSOCKET`.

```ruby
on :fd, file.fileno, flags: FD_READ do
  data = file.read
  # do something with data
  EAT_ALL
end
```

### HexChat::Plugin Helper Methods and Constants

#### Methods

The following instance methods exist in the HexChat::Plugin class:

Method                | Action
----------------------|----------
`away`                | Shortcut to `get_info('away')`, can be assigned to which is a shortcut to `command("away #{string}")`.
`color(fore[, back])` | Produce a mIRC color code.
`command(command)`    | Execute HexChat command (without leading /).
`channel`             | Shortcut to `get_info('channel')`.
`emit_print(event, *args)` | Calls the HexChat emit_print function for the given event and up to 6 args.
`get_info(item)`      | Call the HexChat get_info() function.
`network`             | Shortcut to `get_info('network')`.
`nickcmp(nick1, nick2)` | Compare the given nicks.
`server`              | Shortcut to `get_info('server')`.
`strip(string[, flags)` | Strip given string of mIRC formatting.  Flags may be: `HexChat::STRIP_COLOR`, `HexChat::STRIP_ATTR`, or `HexChat::STRIP_ALL` (default).
`topic`               | Shortcut to `get_info('topic')`, can be assigned to which is a shortcut to `command("topic #{string}")`.
`unhook`              | When called from within a hook, unhook it.
`unregister`          | Unregister this plugin.
`get_prefs(*args)`    | Return HexChat preferences.  If a single item is given, a single item is returned.  If multiple items are given, an array is returned.
`pluginpref_get_int(variable)`        | Get an Integer plugin preference.
`pluginpref_get_str(variable)`        | Get a String plugin preference.
`pluginpref_set_int(variable, value)` | Set an Integer plugin preference.   These are shared among all MRuby plugins, so namespace it accordingly.
`pluginpref_set_str(variable, value)` | Set a String plugin preference.  Same caveat.
`print(*args)`        | Print each of the args to the HexChat window.
`puts`                | Alias of `print`.

#### Constants

The following constants exist in the HexChat module and are included in the HexChat::Plugin class:

##### Formatting Constants

Constant    | Value
------------|-------
`COLORS`    | A hash of color symbols to value mappings.  These vary by IRC client and configuration. :white, :black, :blue, :navy, :green, :light\_red, :brown, :maroon, :purple, :orange, :olive, :yellow, :light\_green, :lime, :cyan, :teal, :light\_cyan, :aqua, :light\_blue, :royal, :pink, :light\_purple, :fuchsia, :grey, :light\_grey, :silver, :default.
`COLOR`     | The mIRC color escape code.
`BOLD`      | The mIRC bold escape code.
`ITALIC`    | The mIRC italic escape code.
`UNDERLINE` | The mIRC underline escape code.
`INVERSE`   | The mIRC inverse escape code.
`RESET`     | The mIRC formatting reset code.

### Contexts

A HexChat context correlates to a displayed window or tab, most often linked to a server and channel or query associated with that server.

Context functions are provided by the `HexChat::Context` class as follows:

Method          | Use
----------------|-----
`::current`     | Return an instance of the current context (that the hook was triggered in, usually).
`::find(server, channel)` | Find an arbitrary context.
`::focused`     | Return an instance of the context the user is viewing.
`#==(other)`    | See if this instance and *other* reference the same HexChat context.
`#set`          | Make this context active.
`#with` *block* | Temporarily set this context and execute *block*.

### Lists

HexChat lists are defined through the `HexChat::List::`*list_name* classes.  These are dynamically generated when the MRuby interface loads. 

Currently implemented are:

List Class                | Contents
--------------------------|----------
`HexChat::List::Channels` | Channels and queries.
`HexChat::List::Dcc`      | Active DCCs
`HexChat::List::Ignore`   | Ignore list
`HexChat::List::Notify`   | Notify list.
`HexChat::List::Users`    | Users in current context.

Methods implemented for each list class:

Method     | Use
-----------|-----
`::fields` | Return a hash, the keys are valid field names as symbols for the list, the values are hashes with keys :name => HexChat internal name of the field, :type => the type of the field (:string, :context, :integer).
`::name`   | Return the name of the list (e.g. `channels`).
`::all`    | Returns an array of hashes. Each hash has keys corresponding to what is returned by `::fields` for this list type.
`::each` *block* \|*row*\|` | Yields each row of the list to *block*.
`::to_a`   | Alias for `::all`.

Field values which internally pointers to HexChat contexts return instances of HexChat::Context.

#### List Flags / Types

Some of the lists use bit fields for flags and numbers for types as follows.  These are mapped to constants in the HexChat module.

##### Channels List

`flags` field:

Constant                    | Meaning
----------------------------|---------
`CHAN_CONNECTED`            | Server is connected.
`CHAN_CONNECTING`           | Server is connecting.
`CHAN_AWAY`                 | User is marked as away here.
`CHAN_END_OF_MOTD`          | End of MOTD received
`CHAN_WHOX`                 | Has WHOX.
`CHAN_IDMSG`                | Has IDMSG.
`CHAN_HIDE_JOIN_PART`       | Joins/Parts are hidden.
`CHAN_HIDE_JOIN_PART_UNSET` | Joins/Parts hiding not set.
`CHAN_BEEP`                 | Beep enabled.
`CHAN_BLINK_TRAY`           | Tray blink enabled.
`CHAN_BLINK_TASKBAR`        | Taskbar blink enabled.
`CHAN_LOGGING`              | Logging enabled.
`CHAN_LOGGING_UNSET`        | Logging not set.
`CHAN_SCROLLBACK`           | Scrollback enabled.
`CHAN_SCROLLBACK_UNSET`     | Scrollback not set.
`CHAN_STRIP`                | Color strip enabled.
`CHAN_STRIP_UNSET`          | Color strip not set.

`type` field:

Constant       | Meaning
---------------|---------
`TYPE_SERVER`  | Is a server message context.
`TYPE_CHANNEL` | Is a channel context.
`TYPE_DIALOG`  | Is a dialog.
`TYPE_NOTICE`  | Is a notice context.
`TYPE_SNOTICE` | Is a snotice context.

##### DCC List

`status` field:

Constant         | Meaning
-----------------|---------
`DCC_QUEUED`     | DCC Queued.
`DCC_ACTIVE`     | DCC active.
`DCC_FAILED`     | DCC failed.
`DCC_DONE`       | DCC done.
`DCC_CONNECTING` | DCC connecting
`DCC_ABORTED`    | DCC aborted.

`type` field:

Constant         | Meaning
-----------------|---------
`TYPE_SEND`      | File send.
`TYPE_RECV`      | File receive.
`TYPE_CHAT_RECV` | Chat receive.
`TYPE_CHAT_SEND` | Chat send.

##### Ignore List

Constant          | Meaning
------------------|---------
`IGNORE_PRIVATE`  | Private messages ignored.
`IGNORE_NOTICE`   | Notices ignored.
`IGNORE_CHANNEL`  | Channel messages ignored.
`IGNORE_CTCP`     | CTCPs ignored.
`IGNORE_INVITE`   | Invites ignored.
`IGNORE_UNIGNORE` | Unignored.
`IGNORE_NOSAVE`   | Don't save this entry.
`IGNORE_DCC`      | Ignore DCCs.


## Internals

The C interface pretty much replicates the raw HexChat plugin functions as methods of the HexChat::Internal class.  These are then beautified by the other classes in the HexChat namespace.

The following modules and classes exist in the interface:

Class/Module        | Lang  | Use
--------------------|-------|-----
`HexChat` (module)  | Mixed | Namespacing and constants
`HexChat::Internal` | Mixed | Ugly internal functions that should not normally be touched.
`HexChat::Internal::Context` | C | Class representing HexChat contexts.
`HexChat::Internal::Hook`    | C | Class representing HexChat hooks.
`HexChat::Internal::List`    | C | Class representing HexChat lists.
`HexChat::Context` | Ruby | Pretty wrapper for `HexChat::Internal::Context`.
`HexChat::Hook`    | Ruby | Pretty wrapper for `HexChat::Internal::Hook`.
`HexChat::List`    | Ruby | Pretty wrapper for `HexChat::Internal::List`.
`HexChat::Plugin`  | Ruby | Plugin base class.
`HexChat::Plugin::Registry` | Ruby | Plugin registry, maintains plugin instances and handles loading/cleanup.
`XChat` (constant) | Ruby | Equal to `HexChat`.

The C code is implemented in `mruby.c` and the Ruby code in `hexchat_mrb_lib.rb`.  When building the plugin, `mrbc` is executed to compile `hexchat_mrb_lib.rb` to a C header file containing MRuby intermediate code, which is then loaded into the interpreter when the plugin starts.

You should probably look at `mruby.c` and `hexchat_mrb_lib.rb` if you want to develop on this or figure out what's going on under the hood.

## FAQ

Q.  Why?

A.  Because I can.  I like writing Ruby code, and I wanted to write Ruby code to make IRC more fun/useful/etc.  I used to write a lot of Perl code for XChat, I've moved on to Ruby for most things, might as well do it here, too.

Q.  I mean why MRuby?

A.  It produces a self-containe plugin, provided you don't get crazy with the gems that have library dependencies.  It's lighter-weight in the grand scheme of things compared to the Perl and Python plugins that come with HexChat.

Q.  This code sucks.

A.  You are welcome to send a pull request.  I wrote this in a few hours over several days without ever having looked at MRuby or embedded any other language before, and my C is pretty damn rusty.  Frankly, I'm happy it works at all.

Q.  There are no tests, you suck!

A.  This code was written using the Entertainment-Driven Development process.  Tests aren't that entertaining for me.

## TODO

### Will

Event Attributes

  - `event_attrs_create`, `event_attrs_free`
  -  `emit_print_attrs`
  - Hooks for print\_attrs and server\_attrs
  - `send_modes`
  - Hook `/LOAD` and `/UNLOAD`

### Maybe

  - Wrap contexts better.
  - Wrap lists better and as a real `Enumerator`.
  - `plugingui`

### Won't

  - `commandf` and `printf` - Ruby string interpolation eliminates the need for these.

## Legal

`mruby.c` and `hexchat_mrb_lib.rb` (c) 2016 by mg^ ("mgcaret").  Some of the C boilerplate was copied from the HexChat Perl and Python plugins.

### License

GPLv2.

