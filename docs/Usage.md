## Maintaining pCloud Console Client

### Run client

The usual way to run client is to type its name in a Unix shell prompt,
followed by pCloud account name as follows:

```sh
$ pcloudcc -u example@myemail.com -p
```

This will start up the process and give you control over it.

You can control its output (by default output is printed to the screen;
you can arrange to have output go into file `pcloud.log` by typing
`>pcloud.log` at the end of the command line), pause and restart the
process (by pressing <kbd>Ctrl</kbd> + <kbd>z</kbd> and typing `fg`), and
kill it (by pressing <kbd>Ctrl</kbd> + <kbd>c</kbd>).

Another way to terminate client is to send a signal eg
`killall -SIGTERM pcloudcc`. Signals with special meaning listened by
client are:

- `SIGINT`: issued if the user sends an interrupt signal
  (<kbd>Ctrl</kbd> + <kbd>c</kbd>)
- `SIGTERM`: sent by kill by default

Any other signals like `SIGQUIT` (<kbd>Ctrl</kbd> + <kbd>\\</kbd>),
`SIGKILL`, etc are handled by your OS. For more see `man 2 signal` and
`man 1 kill`.

To get a help run the client followed by `-h` option. This will ask client to
output short options description:

```sh
$ pcloudcc --help
pCloud Console Client v3.0.0-git
Allowed options:
  -h [ --help ]             produce help message
  -u [ --username ] arg     pCloud account name
  -p [ --password ]         Ask pCloud account password
  -c [ --crypto ]           Ask crypto password
  -y [ --passascrypto ] arg Use user password as crypto password also
  -d [ --daemonize ]        Daemonize the process
  -o [ --commands  ]        Parent stays alive and processes commands
  -m [ --mountpoint ] arg   Mount point where drive to be mounted
  -k [ --commands_only ]    Daemon already started pass only commands
  -n [ --newuser ]          Switch if this is a new user to be registered
  -s [ --savepassword ]     Save password in database
```

Also, there are several commands that the running service can execute. Commands are passed using

```sh
$ pcloudcc -k
```

or  starting the daemon with `-o`. Available commands are:
- `startcrypto <crypto pass>` - starts crypto using given password.
- `stopcrypto` – stops the crypto.
- `finalize` – stops the running daemon.
- `quit`, `q` - exits the current client. Daemon stays alive.

#### Example usage scenario

1. Start the service manually
   ```sh
   $ pcloudcc -u example@myemail.com -p -s
   ```
2. Enter password and use `-s` switch to save the password.
3. Verify that file system starts and mounts normally. If you don't have
   existing user use `-n` switch to register new user:
   ```sh
   $ pcloudcc -u example@myemail.com -p -s -n
   ```
   Notice that a new user may take a while to mount. Please, be patient.
4. Start the daemon service:
   ```sh
   $ pcloudcc -u example@myemail.com -d
   ```
5. Verify the file system is mounted.
6. At that point you can test passing some commands.
   ```sh
   $ pcloudcc -u example@myemail.com -k
   ```
   Or starting the daemon with `-o`. Test unlocking and locking crypto if you
   have subscription for it.
7. Quit the client. Congratulations, your pCloud Console Client works properly.
   You can now add `pcloudcc -u example@myemail.com -d` command in you startup
   scripts  and thous mount the file system on startup.

**Note:** Stopping daemon will break pending background transfers.
Current version of `pcloudcc` doesn't support command to check if there are
pending transfers. Locally cached files are located under `~/.pcloud/Cache`
directory. When there is only one file `~/.pcloud/Cache/cached` (usually big sized)
this mean that all transfers are completed.

### Autostart on system boot

If you want client to start when your operating system boots, the
mechanism varies. It's probably easiet to just follow
[these instructions](https://www.howtogeek.com/228467/how-to-make-a-program-run-at-startup-on-any-computer/)
for setting up autostart. Alternatively, you can try following the instructions below.

#### Linux (systemd)

Traditionally, you must add a command line to the shell script in
`/etc/rc` (which is read by `init` when the system boots), using the `&`
notation to run client in the background. This would suffice for most
situations, but it isn't always possible.

These days, many popular operating systems have a replacement `init`
program with its own configuration language. Example configuration files
for several of these are included in 
[`extras`](https://github.com/sergeyklay/pcloud-console-client/tree/master/extras)
directory, but the most common is probably **systemd**, which reads a service
definition for lines describing command to run at various times. To use
systemd integration the project should be built  with `-DPCLOUD_WITH_SYSTEMD=ON`
flag.  You can autostart client on system boot as follows:

As your own user enable it:

```sh
$ systemctl --user enable pcloudcc@<example@myemail.com>.service
```

Finally, start it:

```sh
$ systemctl --user start pcloudcc@<example@myemail.com>.service
```

Remember to initialize you account first by running:

```sh
$ pcloudcc -u example@myemail.com -p -s
```

### EU region users

In order to connect to the EU region api server the environment variable
`PCLOUD_REGION_EU` has to be set, e.g. like this:

```sh
$ PCLOUD_REGION_EU=true pcloudcc -u example@myemail.com -p -s -n
```

Users with accounts in the EU region will get an login failed error
when connection to the US server and vice versa.
