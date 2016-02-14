# This quick 'n' dirty HexChat MRuby plugin cheats at IRC hangman.
# Generating the wordlist/statistics databases is left as an
# exercise to the reader.  After all, I can't make it *too* easy.
# You need the sqlite, a regexp, and file mrbgems
class HangTite < HexChat::Plugin
  setup do
    # You might need to change some of this.
    # Demonstrates get_info function
    @dir = "#{get_info('configdir')}/mruby/hangtite"
    @db_page_size = 30
    @placeholder = '␣'
    # Demonstrates formatting constants
    @h = "#{color(:lime, :black)} "
    @t = " #{RESET}"
    # Demonstrates pluginpref_get_str
    @enabled = pluginpref_get_str('hangtite_enabled') == 'true'
    @english = 'etaoinsrhdlucmfywgpbvkxqjz' # from ~40,000 words
    @def_stats = Hash.new(@english)
    @guess_nick = nil
    @guess_cons = nil
    @guess_aggr = nil
    msg("HangTite loaded.  Enabled: #{@enabled}")
  end

  # Demonstrates a command hook and pluginpref_set_str
  on :command, 'hangtite', help: 'HANGTITE toggle HangTite & persist setting' do
    @enabled = !@enabled
    msg("HangTite enabled: #{@enabled}")
    pluginpref_set_str('hangtite_enabled', @enabled.to_s)
    EAT_ALL
  end

  # HexChat should be configured to execute /F2 and /F3 on th
  # F2 and F3 keys, respectively.

  on :command, 'f2', help: 'F2 HangTite perform conservative guess' do
    send_guess(@guess_cons)
  end

  on :command, 'f3', help: 'F3 HangTite perform aggressive guess' do
    send_guess(@guess_aggr)
  end

  # Demonstrates the send command function and HexChat preferences
  def send_guess(guess)
    if @enabled
      if @guess_nick && guess
        command "SAY #{@guess_nick}#{get_prefs('completion_suffix')} #{guess}"
      end
      EAT_ALL
    else
      EAT_HEXCHAT
    end
  end

  # Demonstrates a print hook
  on :print, 'Channel Message' do |words|
    make_guess(words) if @enabled
    EAT_NONE
  end

  def make_guess(words)
    m = words[1]
    if m =~ /^(.+)\s+\[(.*)\]\s+(\d+)\/(\d+)\s+\((.+)\)\s*$/
      @guess_nick, @guess_cons, @guess_aggr = [words[0], nil, nil]
      word = Regexp.last_match[1]
      # MRuby doesn't do UTF-8 very nicely, sub in something
      # rather unlikely to be part of a word
      word_t = word.gsub(@placeholder, "\t")
      missed = Regexp.last_match[2]
      used = Regexp.last_match[3].to_i
      total = Regexp.last_match[4].to_i
      category = Regexp.last_match[5]
      file = "#{@dir}/#{category}.db"
      if File.exist?(file)
        begin
          db = SQLite3::Database.new(file)
          full, short, bits, inv = db_get_stats(db, word_t.length)
          if full && short && bits
            stats = remove(full, word_t, missed)
            msg("Stats: #{stats} (#{word_t.length})")
            t1 = Time.now
            words = words_from_db(db, word_t, missed, short, bits, inv)
            tt = (Time.now - t1).to_f.round(4)
            msg("#{words.first(word_t.length > 10 ? 3 : 5).join(' | ')} (#{words.count} returned, #{tt} s)")
            if words.count == 1
              @guess_cons = remove(words[0], word_t, missed)
              @guess_aggr = @guess_cons
            elsif words.count == 2
              @guess_cons = remove(words[0], word_t, missed)
              @guess_aggr = remove(words[1], word_t, missed)
            else
              @guess_cons = stats[0, ((total - used) / 2).ceil]
              @guess_aggr = stats[0, total - used]
            end
          else
            stats = remove(@def_stats[category], word_t, missed)
            msg("Default: #{stats} (no stats)")
            @guess_cons = stats[0, ((total - used) / 2).ceil]
            @guess_aggr = stats[0, total - used]
          end
        rescue SQLite3::Exception => e
          msg "Exception occurred: #{e.message}"
        ensure
          db.close if db
        end
      else
        stats = remove(@def_stats[category], word_t, missed)
        msg("Default: #{stats} (no file for #{category}")
        @guess_cons = stats[0, ((total - used) / 2).ceil]
        @guess_aggr = stats[0, total - used]
      end
      if @guess_nick
        guesses = []
        guesses.push "F2: #{@guess_nick}: #{@guess_cons}" if @guess_cons
        guesses.push "F3: #{@guess_nick}: #{@guess_aggr}" if @guess_aggr
        msg(guesses.join(' | '))
      end
    end
  end

  # Demonstrates puts & colors
  def msg(arg)
    puts "#{@h}#{arg}#{@t}"
  end

  def mask(word, freq_str, bits)
    fs = freq_str[0, bits]
    word_has = 0
    word.downcase.each_char do |l|
      i = fs.index(l)
      word_has |= 1 << i if i && i
    end
    word_has
  end

  def remove(str, *remove)
    s = str.downcase
    remove.each do |r|
      r.downcase.each_char { |c| s.gsub!(c, '') }
    end
    s.split('').uniq.join
  end
  
  def word_masks(word, missed, short, bits, inv)
    has_m = mask(word, short, bits)
    has_neg = bits ^ inv
    has_not_m = mask(missed, short, bits)
    has_not_neg = bits ^ inv
    [has_m, has_neg, has_not_m, has_not_neg]
  end

  def words_from_db(db, word_t, missed, short, bits, inv)
    words = []
    has_m, has_neg, has_not_m, has_not_neg = word_masks(word_t, missed, short, bits, inv)
    word_sql = word_t.gsub("\t", '_')
    missed_rxp = missed.gsub(/[^[:alnum:]]/, '') # Q&D avoid metachars
    word_rxp = word_t.gsub("\t", "[^#{missed_rxp}]")
    filter = missed.length > 0 ? Regexp.new("^#{word_rxp}$") : Regexp.new('.')
    offset = 0
    done = false
    while !done
      new_words = db_get_words(db, word_sql, has_m, has_not_m, has_neg, has_not_neg, @db_page_size, offset)
      done = true if new_words.count < @db_page_size
      new_words.each do |word|
        words.push(word) if filter.match(word)
      end
      offset += @db_page_size
    end
    words
  end
  
  def db_get_stats(db, length)
    full, short, bits, inv = [nil, nil, nil, nil]
    begin
      stm = db.prepare 'SELECT full, short, bits FROM frequencies WHERE length = ?'
      stm.bind_params(length)
      rs = stm.execute
      if row = rs.next
        full, short, bits = row
        inv = (2**bits - 1).to_i
      end
    rescue SQLite3::Exception => e
      msg "Exception occurred: #{e.message}"
    ensure
      stm.close if stm && !stm.closed?
    end
    [full, short, bits, inv]
  end
  
  def db_get_words(db, word_sql, has_m, has_not_m, has_neg, has_not_neg, limit, offset = 0)
    words = []
    begin
      stm = db.prepare 'SELECT word FROM words WHERE length = ? AND word LIKE ? AND (has & ? == ?) AND (has_not & ? == ?) ORDER BY (has & ?) ASC, (has_not & ?) DESC LIMIT ? OFFSET ?'
      stm.bind_params(word_sql.length, word_sql, has_m, has_m, has_not_m, has_not_m, has_neg, has_not_neg, limit, offset)
      rs = stm.execute
      while rs && !rs.eof?
        row = rs.next
        words.push(row.first) if row
      end
    rescue SQLite3::Exception => e
      msg "Exception occurred: #{e.message}"
    ensure
      stm.close if stm && !stm.closed?
    end
    words
  end
  
  # All plugins must register
  register
end
