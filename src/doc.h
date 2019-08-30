/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2019 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#ifdef DOC_CMD

typedef struct doc_cmd_t {
  char const name[16], *args, *sum, *desc;
} doc_cmd_t;

static const doc_cmd_t doc_cmds[] = {

{ "accept", NULL, "Accept the TLS certificate of a hub.",
  "Use this command to accept the TLS certificate of a hub. This command is"
  " used only in the case the keyprint of the TLS certificate of a hub does not"
  " match the keyprint stored in the database."
},
{ "browse", "[[-f] <user>]", "Download and browse someone's file list.",
  "Without arguments, this opens a new tab where you can browse your own file list."
  " Note that changes to your list are not immediately visible in the browser."
  " You need to re-open the tab to get the latest version of your list.\n\n"
  "With arguments, the file list of the specified user will be downloaded (if"
  " it has not been downloaded already) and the browse tab will open once it's"
  " complete. The `-f' flag can be used to force the file list to be (re-)downloaded."
},
{ "clear", NULL, "Clear the display.",
  "Clears the log displayed on the screen. Does not affect the log files in any way."
  " Ctrl+l is a shortcut for this command."
},
{ "close", NULL, "Close the current tab.",
  "Close the current tab. When closing a hub tab, you will be disconnected from"
  " the hub and all related userlist and PM tabs will also be closed. Alt+c is"
  " a shortcut for this command."
},
{ "connect", "[<address>]", "Connect to a hub.",
  "Initiate a connection with a hub. If no address is specified, will connect"
  " to the hub you last used on the current tab. The address should be in the"
  " form of `protocol://host:port/' or `host:port'. The `:port' part is in both"
  " cases optional and defaults to :411. The following protocols are"
  " recognized: dchub, nmdc, nmdcs, adc, adcs. When connecting to an nmdcs or"
  " adcs hub and the SHA256 keyprint is known, you can attach this to the url as"
  " `?kp=SHA256/<base32-encoded-keyprint>'\n\n"
  "Note that this command can only be used on hub tabs. If you want to open a new"
  " connection to a hub, you need to use /open first. For example:\n\n"
  "  /open testhub\n"
  "  /connect dchub://dc.some-test-hub.com/\n\n"
  "See the /open command for more information."
},
{ "connections", NULL, "Open the connections tab.",
  NULL
},
{ "delhub", "<name>", "Remove a hub from the configuration",
  NULL
},
{ "disconnect", NULL, "Disconnect from a hub.",
  NULL
},
{ "gc", NULL, "Perform some garbage collection.",
  "Cleans up unused data and reorganizes existing data to allow more efficient"
  " storage and usage. Currently, this commands removes unused hash data, does"
  " a VACUUM on db.sqlite3, removes unused files in inc/ and old files in"
  " fl/.\n\n"
  "This command may take some time to complete, and will fully block ncdc while"
  " it is running. It is recommended to run this command every once in a while."
  " Every month is a good interval. Note that when ncdc says that it has"
  " completed this command, it's lying to you. Ncdc will still run a few large"
  " queries on the background, which may take up to a minute to complete."
},
{ "grant", "[-list|<user>]", "Grant someone a slot.",
  "Grant someone a slot. This allows the user to download from you even if you"
  " have no free slots.  The slot will remain granted until the /ungrant"
  " command is used, even if ncdc has been restarted in the mean time.\n\n"
  "To get a list of users whom you have granted a slot, use `/grant' without"
  " arguments or with `-list'. Be warned that using `/grant' without arguments on"
  " a PM tab will grant the slot to the user you are talking with. Make sure to"
  " use `-list' in that case.\n\n"
  "Note that a granted slot is specific to a single hub. If the same user is"
  " also on other hubs, he/she will not be granted a slot on those hubs."
},
{ "help", "[<command>|set <key>|keys [<section>]]", "Request information on commands.",
  "To get a list of available commands, use /help without arguments.\n"
  "To get information on a particular command, use /help <command>.\n"
  "To get information on a configuration setting, use /help set <setting>.\n"
  "To get help on key bindings, use /help keys.\n"
},
{ "hset", "[<key> [<value>]]", "Get or set per-hub configuration variables.",
  "Get or set per-hub configuration variables. Works equivalent to the `/set'"
  " command, but can only be used on hub tabs. Use `/hunset' to reset a"
  " variable back to its global value."
},
{ "hunset", "[<key>]", "Unset a per-hub configuration variable.",
  "This command can be used to reset a per-hub configuration variable back to"
  " its global value."
},
{ "kick", "<user>", "Kick a user from the hub.",
  "Kick a user from the hub. This command only works on NMDC hubs, and you need"
  " to be an OP to be able to use it."
},
{ "listen", NULL, "List currently opened ports.",
  NULL,
},
{ "me", "<message>", "Chat in third person.",
  "This allows you to talk in third person. Most clients will display your message as something like:\n\n"
  "  ** Nick is doing something\n\n"
  "Note that this command only works correctly on ADC hubs. The NMDC protocol"
  " does not have this feature, and your message will be sent as-is, including the /me."
},
{ "msg", "<user> [<message>]", "Send a private message.",
  "Send a private message to a user on the currently opened hub. If no"
  " message is given, the tab will be opened but no message will be sent."
},
{ "nick", "[<nick>]", "Alias for `/hset nick' on hub tabs, and `/set nick' otherwise.",
  NULL
},
{ "open", "[-n] [<name>] [<address>]", "Open a new hub tab and connect to the hub.",
  "Without arguments, list all hubs known by the current configuration."
  " Otherwise, this opens a new tab to use for a hub. The name is a (short)"
  " personal  name you use to identify the hub, and will be used for storing"
  " hub-specific  configuration.\n\n"
  "If you have specified an address or have previously connected to a hub"
  " from a tab with the same name, /open will automatically connect to"
  " the hub. Use the `-n' flag to disable this behaviour.\n\n"
  "See /connect for more information on connecting to a hub."
},
{ "password", "<password>", "Send your password to the hub.",
  "This command can be used to send a password to the hub without saving it to"
  " the database. If you wish to login automatically without having to type"
  " /password every time, use '/hset password <password>'. Be warned, however,"
  " that your password will be saved unencrypted in that case."
},
{ "pm", "<user> [<message>]", "Alias for /msg",
  NULL
},
{ "queue", NULL, "Open the download queue.",
  NULL
},
{ "quit", NULL, "Quit ncdc.",
  "Quit ncdc."
},
{ "reconnect", NULL, "Shortcut for /disconnect and /connect",
  "Reconnect to the hub. When your nick or the hub encoding have been changed,"
  " the new settings will be used after the reconnect.\n\n"
  "This command can also be used on the main tab, in which case all connected"
  " hubs will be reconnected."
},
{ "refresh", "[<path>]", "Refresh file list.",
  "Initiates share refresh. If no argument is given, the complete list will be"
  " refreshed. Otherwise only the specified directory will be refreshed. The"
  " path argument can be either an absolute filesystem path or a virtual path"
  " within your share."
},
{ "say", "<message>", "Send a chat message.",
  "Sends a chat message to the current hub or user. You normally don't have to"
  " use the /say command explicitly, any command not staring with '/' will"
  " automatically imply `/say <command>'. For example, typing `hello.' in the"
  " command line is equivalent to `/say hello.'. Using the /say command"
  " explicitly may be useful to send message starting with '/' to the chat, for"
  " example `/say /help is what you are looking for'."
},
{ "search", "[options] <query>", "Search for files.",
  "Performs a file search, opening a new tab with the results.\n\n"
  "Available options:\n\n"
  "  -hub      Search the current hub only. (default)\n"
  "  -all      Search all connected hubs, except those with `chat_only' set.\n"
  "  -le  <s>  Size of the file must be less than <s>.\n"
  "  -ge  <s>  Size of the file must be larger than <s>.\n"
  "  -t   <t>  File must be of type <t>. (see below)\n"
  "  -tth <h>  TTH root of this file must match <h>.\n\n"
  "File sizes (<s> above) accept the following suffixes: G (GiB), M (MiB) and K (KiB).\n\n"
  "The following file types can be used with the -t option:\n\n"
  "  1  any      Any file or directory. (default)\n"
  "  2  audio    Audio files.\n"
  "  3  archive  (Compressed) archives.\n"
  "  4  doc      Text documents.\n"
  "  5  exe      Windows executables.\n"
  "  6  img      Image files.\n"
  "  7  video    Video files.\n"
  "  8  dir      Directories.\n\n"
  "Note that file type matching is done using file extensions, and is not very reliable."
},
{ "set", "[<key> [<value>]]", "Get or set global configuration variables.",
  "Get or set global configuration variables. Use without arguments to get a"
  " list of all global settings and their current value. Glob-style pattern"
  " matching on the settings is also possible. Use, for example, `/set color*'"
  " to list all color-related settings.\n\n"
  "See the `/unset' command to change a setting back to its default, and the"
  " `/hset' command to manage configuration on a per-hub basis. Changes to the"
  " settings are automatically saved to the database, and will not be lost after"
  " restarting ncdc.\n\n"
  "To get information on a particular setting, use `/help set <key>'."
},
{ "share", "[<name> <path>]", "Add a directory to your share.",
  "Use /share without arguments to get a list of shared directories.\n"
  "When called with a name and a path, the path will be added to your share."
  " Note that shell escaping may be used in the name. For example, to add a"
  " directory with the name `Fun Stuff', you could do the following:\n\n"
  "  /share \"Fun Stuff\" /path/to/fun/stuff\n\n"
  "Or:\n\n"
  "  /share Fun\\ Stuff /path/to/fun/stuff\n\n"
  "The full path to the directory will not be visible to others, only the name"
  " you give it will be public. An initial `/refresh' is done automatically on"
  " the added directory."
},
{ "ungrant", "[<user>]", "Revoke a granted slot.",
  NULL
},
{ "unset", "[<key>]", "Unset a global configuration variable.",
  "This command can be used to reset a global configuration variable back to its default value."
},
{ "unshare", "[<name>]", "Remove a directory from your share.",
  "To remove a single directory from your share, use `/unshare <name>', to"
  " remove all directories from your share, use `/unshare /'.\n\n"
  "Note that the hash data associated with the removed files will remain in the"
  " database. This allows you to re-add the files to your share without needing"
  " to re-hash them. The downside is that the database file may grow fairly large"
  " with unneeded information. See the `/gc' command to clean that up."
},
{ "userlist", NULL, "Open the user list.",
  "Opens the user list of the currently selected hub. Can also be accessed using Alt+u."
},
{ "version", NULL, "Display version information.",
  NULL
},
{ "whois", "<user>", "Locate a user in the user list.",
  "This will open the user list and select the given user."
},

{ "" }
};

#endif // DOC_CMD



#ifdef DOC_SET

typedef struct doc_set_t {
  char const *name;
  int hub;
  char const *type, *desc;
} doc_set_t;

static const doc_set_t doc_sets[] = {

{ "active", 1, "<boolean>",
  "Enables or disables active mode. You may have to configure your router"
  " and/or firewall for this to work, see the `active_ip' and `active_port'"
  " settings for more information."
},
{ "active_ip", 1, "<string>",
  "Your public IP address for use in active mode. If this is not set or set to"
  " '0.0.0.0' for IPv4 or '::' for IPv6, then ncdc will try to automatically"
  " get your IP address from the hub. If you do set this manually, it is"
  " important that other clients can reach you using this IP address. If you"
  " connect to a hub on the internet, this should be your internet (WAN) IP."
  " Likewise, if you connect to a hub on your LAN, this should be your LAN"
  " IP.\n\n"
  "Both an IPv4 and an IPv6 address are set by providing two IP addresses"
  " separated with a comma. When unset, '0.0.0.0,::' is assumed. Only the IP"
  " version used to connect to the hub is used. That is, if you connect to an"
  " IPv6 hub, then the configured IPv6 address is used and the IPv4 address is"
  " ignored.\n\n"
  "When set to the special value `local', ncdc will automatically get your IP"
  " address from the local network interface that is used to connect to the"
  " hub. This option should only be used if there is no NAT between you and"
  " the hub, because this will give the wrong IP if you are behind a NAT."
},
{ "active_port", 1, "<integer>",
  "The listen port for incoming connections in active mode. Set to `0' to"
  " automatically assign a random port. This setting is by default also used"
  " for the UDP port, see the `active_tls_port' settings to change that. If you"
  " are behind a router or firewall, make sure that you have configured it to"
  " forward and allow these ports."
},
{ "active_udp_port", 1, "<integer>",
  "The listen port for incoming UDP connections in active mode. Defaults to the"
  " `active_port' setting, or to a random number if `active_port' is not set."
},
{ "adc_blom", 1, "<boolean>",
  "Whether to support the BLOM extension on ADC hubs. This may decrease the"
  " bandwidth usage on the hub connection, in exchange for a bit of"
  " computational overhead. Some hubs require this setting to be enabled. This"
  " setting requires a reconnect with the hub to be active."
},
{ "autoconnect", 1, "<boolean>",
  "Set to true to automatically connect to the current hub when ncdc starts up."
},
{ "autorefresh", 0, "<interval>",
  "The time between automatic file refreshes. Recognized suffices are 's' for"
  " seconds, 'm' for minutes, 'h' for hours and 'd' for days. Set to 0 to"
  " disable automatically refreshing the file list. This setting also"
  " determines whether ncdc will perform a refresh on startup. See the"
  " `/refresh' command to manually refresh your file list."
},
{ "backlog", 1, "<integer>",
  "When opening a hub or PM tab, ncdc can load a certain amount of lines from"
  " the log file into the log window. Setting this to a positive value enables"
  " this feature and configures the number of lines to load. Note that, while"
  " this setting can be set on a per-hub basis, PM windows will use the global"
  " value (global.backlog)."
},
{ "chat_only", 1, "<boolean>",
  "Set to true to indicate that this hub is only used for chatting. That is,"
  " you won't or can't download from it. This setting affects the /search"
  " command when it is given the -all option."
},
// Note: the setting list isn't alphabetic here, but in a more intuitive order
{ "color_*", 0, "<color>",
  "The settings starting with the `color_' prefix allow you to change the"
  " interface colors. The following is a list of available color settings:\n\n"
  "  list_default  - default item in a list\n"
  "  list_header   - header of a list\n"
  "  list_select   - selected item in a list\n"
  "  log_default   - default log color\n"
  "  log_time      - the time prefix in log messages\n"
  "  log_nick      - default nick color\n"
  "  log_highlight - nick color of a highlighted line\n"
  "  log_ownnick   - color of your own nick\n"
  "  log_join      - color of join messages\n"
  "  log_quit      - color of quit messages\n"
  "  separator     - the list separator/footer bar\n"
  "  tab_active    - the active tab in the tab list\n"
  "  tabprio_low   - low priority tab notification color\n"
  "  tabprio_med   - medium priority tab notification color\n"
  "  tabprio_high  - high priority tab notification color\n"
  "  title         - the title bar\n"
  "\n"
  "The actual color value can be set with a comma-separated list of color names"
  " and/or attributes. The first color in the list is the foreground color, the"
  " second color is used for the background. When the fore- or background color"
  " is not specified, the default colors of your terminal will be used.\n"
  "The following color names can be used: black, blue, cyan, default, green,"
  " magenta, red, white and yellow.\n"
  "The following attributes can be used: bold, blink, reverse and underline.\n"
  "The actual color values displayed by your terminal may vary. Adding the"
  " `bold' attribute usually makes the foreground color appear brighter as well."
},
{ "connection", 1, "<string>",
  "Set your upload speed. This is just an indication for other users in the hub"
  " so that they know what speed they can expect when downloading from you. The"
  " actual format you can use here may vary, but it is recommended to set it to"
  " either a plain number for Mbit/s (e.g. `50' for 50 mbit) or a number with a"
  " `KiB/s' indicator (e.g. `2300 KiB/s'). On ADC hubs you must use one of the"
  " previously mentioned formats, otherwise no upload speed will be"
  " broadcasted. This setting is broadcasted as-is on NMDC hubs, to allow for"
  " using old-style connection values (e.g. `DSL' or `Cable') on hubs that"
  " require this.\n\n"
  "This setting is ignored if `upload_rate' has been set. If it is, that value"
  " is broadcasted instead."
},
{ "description", 1, "<string>",
  "A short public description that will be displayed in the user list of a hub."
},
{ "disconnect_offline", 1, "<boolean>",
  "Automatically disconnect any upload or download transfers when a user leaves"
  " the hub, or when you leave the hub. Setting this to `true' ensures that you"
  " are only connected with people who are online on the same hubs as you are."
},
{ "download_dir", 0, "<path>",
  "The directory where finished downloads are moved to. Finished downloads are"
  " by default stored in <session directory>/dl/. It is possible to set this to"
  " a location that is on a different filesystem than the incoming directory,"
  " but doing so is not recommended: ncdc will block when moving the completed"
  " files to their final destination."
},
{ "download_exclude", 0, "<regex>",
  "When recursively adding a directory to the download queue - by pressing `d'"
  " on a directory in the file list browser - any item in the selected"
  " directory with a name that matches this regular expression will not be"
  " added to the download queue.\n\n"
  "This regex is not checked when adding individual files from either the file"
  " list browser or the search results."
},
{ "download_rate", 0, "<speed>",
  "Maximum combined transfer rate of all downloads. The total download speed"
  " will be limited to this value. The suffixes `G', 'M', and 'K' can be used"
  " for GiB/s, MiB/s and KiB/s, respectively. Note that, similar to upload_rate,"
  " TCP overhead are not counted towards this limit, so the actual bandwidth"
  " usage might be a little higher."
},
{ "download_segment", 0, "<size>",
  "Minimum segment size to use when requesting file data from another user."
  " Set to 0 to disable segmented downloading."
},
{
  "download_shared", 0, "<boolean>",
  "Whether to download files which are already present in your share. When this"
  " is set to `false', adding already shared files results in a UI message"
  " instead of adding the file to the download queue."
},
{ "download_slots", 0, "<integer>",
  "Maximum number of simultaneous downloads."
},
{ "email", 1, "<string>",
  "Your email address. This will be displayed in the user list of the hub, so"
  " only set this if you want it to be public."
},
{ "encoding", 1, "<string>",
  "The character set/encoding to use for hub and PM messages. This setting is"
  " only used on NMDC hubs, ADC always uses UTF-8. Some common values are:\n\n"
  "  CP1250      (Central Europe)\n"
  "  CP1251      (Cyrillic)\n"
  "  CP1252      (Western Europe)\n"
  "  ISO-8859-7  (Greek)\n"
  "  KOI8-R      (Cyrillic)\n"
  "  UTF-8       (International)"
},
{ "filelist_maxage", 0, "<interval>",
  "The maximum age of a downloaded file list. If a file list was downloaded"
  " longer ago than the configured interval, it will be removed from the cache"
  " (the fl/ directory) and subsequent requests to open the file list will"
  " result in the list being downloaded from the user again. Recognized"
  " suffices are 's' for seconds, 'm' for minutes, 'h' for hours and 'd' for"
  " days. Set to 0 to disable the cache altogether."
},
{ "flush_file_cache", 0, "<none|upload|download|hash>[,...]",
  "Tell the OS to flush the file (disk) cache for file contents read while"
  " hashing and/or uploading or written to while downloading. On one hand, this"
  " will avoid trashing your disk cache with large files and thus improve the"
  " overall responsiveness of your system. On the other hand, ncdc may purge any"
  " shared files from the cache, even if they are still used by other"
  " applications. In general, it is a good idea to enable this if you also use"
  " your system for other things besides ncdc, you share large files (>100MB)"
  " and people are not constantly downloading the same file from you."
},
{ "geoip_cc", 0, "<path>|disabled",
  "Path to the GeoIP2 Country database file (GeoLite2-Country.mmdb), or"
  " 'disabled' to disable GeoIP lookups. The database can be downloaded "
  " from https://dev.maxmind.com/geoip/geoip2/geolite2/."
},
{ "hash_rate", 0, "<speed>",
  "Maximum file hashing speed. See the `download_rate' setting for allowed"
  " formats for this setting."
},
{ "hubname", 1, "<string>",
  "The name of the currently opened hub tab. This is a user-assigned name, and"
  " is only used within ncdc itself. This is the same name as given to the"
  " `/open' command."
},
{ "incoming_dir", 0, "<path>",
  "The directory where incomplete downloads are stored. This setting can only"
  " be changed when the download queue is empty. Also see the download_dir"
  " setting."
},
{ "local_address", 1, "<string>",
  "Specifies the address of the local network interface to use for connecting"
  " to the outside and for accepting incoming connections in active mode."
  " Both an IPv4 and an IPv6 address are set by providing two IP addresses"
  " separated with a comma. When unset, '0.0.0.0,::' is assumed.\n\n"
  "If no IPv4 address is specified, '0.0.0.0' is added automatically."
  " Similarly, if no IPv6 address is specified, '::' is added automatically. The"
  " address that is actually used depends on the IP version actually used. That"
  " is, if you're on an IPv6 hub, then ncdc will listen on the specified IPv6"
  " address. Note that, even if the hub you're on is on IPv6, ncdc may still try"
  " to connect to another client over IPv4, at which point the socket will be"
  " bound to the configured IPv4 address."
},
{ "log_debug", 0, "<boolean>",
  "Log debug messages to stderr.log in the session directory. It is highly"
  " recommended to enable this setting if you wish to debug or hack ncdc. Be"
  " warned, however, that this may generate a lot of data if you're connected"
  " to a large hub."
},
{ "log_downloads", 0, "<boolean>",
  "Log downloaded files to transfers.log."
},
{ "log_hubchat", 1, "<boolean>",
  "Log the main hub chat. Note that changing this requires any affected hub"
  " tabs to be closed and reopened before the change is effective."
},
{ "log_uploads", 0, "<boolean>",
  "Log file uploads to transfers.log."
},
{
  "max_ul_per_user", 0, "<integer>",
  "The maximum number of simultaneous upload connections to one user."
},
{ "minislots", 0, "<integer>",
  "Set the number of available minislots. A `minislot' is a special slot that"
  " is used when all regular upload slots are in use and someone is requesting"
  " your filelist or a small file. In this case, the other client automatically"
  " applies for a minislot, and can still download from you as long as not all"
  " minislots are in use. What constitutes a `small' file can be changed with"
  " the `minislot_size' setting. Also see the `slots' configuration setting and"
  " the `/grant' command."
},
{ "minislot_size", 0, "<integer>",
  "The maximum size of a file that may be downloaded using a `minislot', in"
  " KiB. See the `minislots' setting for more information."
},
{ "nick", 1, "<string>",
  "Your nick. Nick changes are only visible on newly connected hubs, use the "
  " `/reconnect' command to use your new nick immediately. Note that it is"
  " highly discouraged to change your nick on NMDC hubs. This is because"
  " clients downloading from you have no way of knowing that you changed your"
  " nick, and therefore can't immediately continue to download from you."
},
{ "notify_bell", 0, "<disable|low|medium|high>",
  "When enabled, ncdc will send a bell to your terminal when a tab indicates a"
  " notification. The notification types are:\n\n"
  "  high   - Messages directed to you (PM or highlight in hub chat),\n"
  "  medium - Regular hub chat,\n"
  "  low    - User joins/quits, new search results, etc.\n"
  "\nHow a \"bell\" (or \"beep\" or \"alert\", whatever you prefer to call it)"
  " manifests itself depends on your terminal. In some setups, this generates an"
  " audible system bell. In other setups it can makes your terminal window flash"
  " or do other annoying things to get your attention.  And in some setups it is"
  " ignored completely."
},
{ "password", 1, "<string>",
  "Sets your password for the current hub and enables auto-login on connect. If"
  " you just want to login to a hub without saving your password, use the"
  " `/password' command instead. Passwords are saved unencrypted in the config"
  " file."
},
{ "reconnect_timeout", 1, "<interval>",
  "The time to wait before automatically reconnecting to a hub. Set to 0 to"
  " disable automatic reconnect."
},
{ "sendfile", 0, "<boolean>",
  "Whether or not to use the sendfile() system call to upload files, if"
  " supported. Using sendfile() allows less resource usage while uploading, but"
  " may not work well on all systems."
},
{ "share_emptydirs", 0, "<boolean>",
  "Share empty directories. When disabled (the default), empty directories in"
  " your share will not be visible to others. This also affects empty"
  " directories containing only empty directories, etc. A file list refresh is"
  " required for this setting to be effective."
},
{ "share_exclude", 0, "<regex>",
  "Any file or directory with a name that matches this regular expression will"
  " not be shared. A file list refresh is required for this setting to be"
  " effective."
},
{ "share_hidden", 0, "<boolean>",
  "Whether to share hidden files and directories. A `hidden' file or directory"
  " is one of which the file name starts with a dot. (e.g. `.bashrc'). A file"
  " list refresh is required for this setting to be effective."
},
{ "share_symlinks", 0, "<boolean>",
  "Whether to follow symlinks in shared directories. When disabled (default),"
  " ncdc will never share any files outside of the directory you specified. When"
  " enabled, any symlinks in your shared directories will be followed, even"
  " when they point to a directory outside your share."
},
{
  "show_free_slots", 1, "<boolean>",
  "When set to true, [n sl] will be prepended to your description, where n is"
  " the number of currently available upload slots."
},
{ "show_joinquit", 1, "<boolean>",
  "Whether to display join/quit messages in the hub chat."
},
{ "slots", 0, "<integer>",
  "The number of upload slots. This determines for the most part how many"
  " people can download from you simultaneously. It is possible that this limit"
  " is exceeded in certain circumstances, see the `minislots' setting and the"
  " `/grant' command."
},
{ "sudp_policy", 1, "<disabled|allow|prefer>",
  "Set the policy for sending or receiving encrypted UDP search results. When"
  " set to `disabled', all UDP search results will be sent and received in"
  " plain text. Set this to `allow' to let ncdc reply with encrypted search"
  " results if the other client requested it. `prefer' will also cause ncdc"
  " itself to request encryption.\n\n"
  "Note that, regardless of this setting, encrypted UDP search results are only"
  " used on ADCS hubs. They will never be sent on NMDC or non-TLS ADC hubs. Also"
  " note that, even if you set this to `prefer', encryption is still only used"
  " when the client on the other side of the connection also supports it."
},
{ "tls_policy", 1, "<disabled|allow|prefer>",
  "Set the policy for secure client-to-client connections. Setting this to"
  " `disabled' disables TLS support for client connections, but still allows"
  " you to connect to TLS-enabled hubs. `allow' will allow the use of TLS if"
  " the other client requests this, but ncdc itself will not request TLS when"
  " connecting to others. Setting this to `prefer' tells ncdc to also request"
  " TLS when connecting to others.\n\n"
  "The use of TLS for client connections usually results in less optimal"
  " performance when uploading and downloading, but is quite effective at"
  " avoiding protocol-specific traffic shaping that some ISPs may do. Also note"
  " that, even if you set this to `prefer', TLS will only be used if the"
  " connecting party also supports it."
},
{ "tls_priority", 0, "<string>",
  "Set the GnuTLS priority string used for all TLS-enabled connections. See the"
  " \"Priority strings\" section in the GnuTLS manual for details on what this"
  " does and how it works. Currently it is not possible to set a different"
  " priority string for different types of connections (e.g. hub or"
  " incoming/outgoing client connections)."
},
{ "ui_time_format", 0, "<string>",
  "The format of the time displayed in the lower-left of the screen. Set `-' to"
  " not display a time at all. The string is passed to the Glib"
  " g_date_time_format() function, which accepts roughly the same formats as"
  " strftime(). Check out the strftime(3) man page or the Glib documentation"
  " for more information. Note that this setting does not influence the"
  " date/time format used in other places, such as the chat window or log"
  " files."
},
{ "upload_rate", 0, "<speed>",
  "Maximum combined transfer rate of all uploads. See the `download_rate'"
  " setting for more information on rate limiting. Note that this setting also"
  " overrides any `connection' setting."
},

{ NULL }
};

#endif // DOC_SET



#ifdef DOC_KEY

// There is some redundancy here with the actual keys used in the switch()
// statements of each window/widget. But the methods for synchronizing the keys
// aren't going to worth it I'm afraid.

// Don't want to redirect people to the global keybindings for these four lines.
#define LISTING_KEYS \
  "Up/Down      Select one item up/down.\n"\
  "k/j          Select one item up/down.\n"\
  "PgUp/PgDown  Select one page of items up/down.\n"\
  "End/Home     Select last/first item in the list.\n"

#define SEARCH_KEYS \
  "/            Start incremental regex search (press Return to stop editing).\n"\
  ",/.          Search next / previous.\n"

typedef struct doc_key_t {
  char const *sect, *title, *desc;
} doc_key_t;

static const doc_key_t doc_keys[] = {

{ "global", "Global key bindings",
  "Alt+j        Open previous tab.\n"
  "Alt+k        Open next tab.\n"
  "Alt+h        Move current tab left.\n"
  "Alt+l        Move current tab right.\n"
  "Alt+a        Move tab with recent activity.\n"
  "Alt+<num>    Open tab with number <num>.\n"
  "Alt+c        Close current tab.\n"
  "Alt+n        Open the connections tab.\n"
  "Alt+q        Open the download queue tab.\n"
  "Alt+o        Open own file list.\n"
  "Alt+r        Refresh file list.\n"
  "\n"
  "Keys for tabs with a log window:\n"
  "Ctrl+l       Clear current log window.\n"
  "PgUp         Scroll the log backward.\n"
  "PgDown       Scroll the log forward.\n"
  "\n"
  "Keys for tabs with a text input line:\n"
  "Left/Right   Move cursor one character left or right.\n"
  "End/Home     Move cursor to the end / start of the line.\n"
  "Up/Down      Scroll through the command history.\n"
  "Tab          Auto-complete current command, nick or argument.\n"
  "Alt+b        Move cursor one word backward.\n"
  "Alt+f        Move cursor one word forward.\n"
  "Backspace    Delete character before cursor.\n"
  "Delete       Delete character under cursor.\n"
  "Ctrl+w       Delete to previous space.\n"
  "Alt+d        Delete to next space.\n"
  "Ctrl+k       Delete everything after cursor.\n"
  "Ctrl+u       Delete entire line."
},
{ "browse", "File browser",
  LISTING_KEYS
  SEARCH_KEYS
  "Right/l      Open selected directory.\n"
  "Left/h       Open parent directory.\n"
  "t            Toggle sorting directories before files.\n"
  "s            Order by file size.\n"
  "n            Order by file name.\n"
  "d            Add selected file/directory to the download queue.\n"
  "m            Match selected item with the download queue.\n"
  "M            Match entire file list with the download queue.\n"
  "a            Search for alternative download sources."
},
{ "connections", "Connection list",
  LISTING_KEYS
  "d            Disconnect selected connection.\n"
  "i/Return     Toggle information box.\n"
  "f            Find user in user list.\n"
  "m            Send a PM to the selected user.\n"
  "q            Find file in download queue.\n"
  "b/B          Browse the selected user's list, B to force a redownload."
},
{ "queue", "Download queue",
  LISTING_KEYS
  "K/J          Select one user up/down.\n"
  "f            Find user in user list.\n"
  "c            Find connection in the connection list.\n"
  "a            Search for alternative download sources.\n"
  "d            Remove selected file from the queue.\n"
  "+/-          Increase/decrease priority.\n"
  "i/Return     Toggle user list.\n"
  "r            Remove selected user for this file.\n"
  "R            Remove selected user from all files in the download queue.\n"
  "x            Clear error state for the selected user for this file.\n"
  "X            Clear error state for the selected user for all files.\n"
  "\n"
  "Note: when an item in the queue has `ERR' indicated in the\n"
  "priority column, you have two choices: You can remove the\n"
  "item from the queue using `d', or attempt to continue the\n"
  "download by increasing its priority using `+'."
},
{ "search", "Search results tab",
  LISTING_KEYS
  "f            Find user in user list.\n"
  "b/B          Browse the selected user's list, B to force a redownload.\n"
  "d            Add selected file to the download queue.\n"
  "h            Toggle hub column visibility.\n"
  "u            Order by username.\n"
  "s            Order by file size.\n"
  "l            Order by free slots.\n"
  "n            Order by file name.\n"
  "m            Match selected item with the download queue.\n"
  "M            Match all search results with the download queue.\n"
  "q            Match selected users' list with the download queue.\n"
  "Q            Match all matched users' lists with the download queue.\n"
  "a            Search for alternative download sources."
},
{ "userlist", "User list tab",
  LISTING_KEYS
  SEARCH_KEYS
  "o            Toggle sorting OPs before others.\n"
  "s/S          Order by share size.\n"
  "u/U          Order by username.\n"
  "t/T          Toggle visibility / order by tag column.\n"
  "e/E          Toggle visibility / order by email column.\n"
  "c/C          Toggle visibility / order by connection column.\n"
  "p/P          Toggle visibility / order by IP column.\n"
  "i/Return     Toggle information box.\n"
  "m            Send a PM to the selected user.\n"
  "g            Grant a slot to the selected user.\n"
  "b/B          Browse the selected users' list, B to force a redownload.\n"
  "q            Match selected users' list with the download queue."
},

{ NULL }
};

#undef LISTING_KEYS
#undef SEARCH_KEYS

#endif // DOC_KEY
