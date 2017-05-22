# passh

**NOTE:** The pty related code is stolen from the [APUE book][apue].

  [apue]: http://www.apuebook.com/

## compile

    $ cc -o passh passh.c
    
## usage 

```
Usage: passh [OPTION]... COMMAND...

  -c <N>          Send at most <N> passwords (0 means infinite. default: 0)
  -C              Exit if prompted for the <N+1>th password
  -h              Help
  -i              Case insensitive for password prompt matching
  -n              Nohup the child (e.g. used for `ssh -f')
  -p <password>   The password (default: `password')
  -p env:<var>    Read password from env var
  -p file:<file>  Read password from file
  -P <prompt>     Regexp (BRE) for the password prompt (default: `[Pp]assword: \{0,1\}$')
  -l <file>       Save data written to the pty
  -L <file>       Save data read from the pty
  -t <timeout>    Timeout waiting for next password prompt (0 means no timeout. default: 0)
  -T              Exit if timed out waiting for password prompt

Report bugs to Clark Wang <dearvoid@gmail.com>
```

## supported platforms

Tested on:

* OpenWRT 15.05.1, ramips/mt7620 (on [Newifi Mini, or Lenovo Y1 v1][newifi])
* Debian Linux 8, x86_64 (Jessie)
* macOS 10.12 (Sierra)
* Cygwin, x86_64 (on Windows 7)

  [newifi]: https://wiki.openwrt.org/toh/lenovo/lenovo_y1_v1

## why i wrote passh

1. I got a `Newifi Mini` router and installed `OpenWRT` on it. I want the router to be my `SOCKS` proxy so I run `ssh -D 8888 user@host` at boot time but the SSH server only supports password auth. On Linux I would use `Expect` to automate `ssh` but `OpenWRT` does not install `Expect` by default and my router does not have enough storage for the extra `Tcl` and `Expect` packages.

1. Then I tried [`sshpass`][sshpass] but `sshpass` seems more like a nice hack and it's *broken* by design. See following example on a Linux system:

        $ tty
        /dev/pts/18                                      // now we're on pts/18
        $ sshpass bash --norc
        bash: cannot set terminal process group (-1): Inappropriate ioctl for device
        bash: no job control in this shell
        bash-4.4# tty
        /dev/pts/18                                      // the bash's stdin is also connected to pts/18
        bash-4.4# ps p $$
           PID TTY      STAT   TIME COMMAND
         37151 pts/36   Ss+    0:00 bash --norc          // but the controlling terminal is pts/36
        bash-4.4# ps t pts/36
           PID TTY      STAT   TIME COMMAND
         37151 pts/36   Ss+    0:00 bash --norc
         37154 pts/36   R+     0:00 ps t pts/36
        bash-4.4#
        
     Now let's try `passh`:
 
        $ tty
        /dev/pts/18                                      // now we're on pts/18
        $ passh bash --norc
        bash-4.4# tty
        /dev/pts/36                                      // the bash's stdin is connected to the new pts/36
        bash-4.4# ps p $$
           PID TTY      STAT   TIME COMMAND
         37159 pts/36   Ss     0:00 bash --norc          // pts/36 is its controlling terminal
        bash-4.4# ps t pts/36
           PID TTY      STAT   TIME COMMAND
         37159 pts/36   Ss     0:00 bash --norc
         37162 pts/36   R+     0:00 ps t pts/36
        bash-4.4#

  [sshpass]: https://sourceforge.net/projects/sshpass/

## examples

1. `sshpass` is *better* in its own way.

    For example, you can use `rsync` + `sshpass` like this:
    
        $ rsync -e 'sshpass -p password ssh' file user@host:/dir
        
    But with `passh` you have to:
    
        $ passh -p password rsync -e ssh file user@host:/dir
        
    Another example, with `sshpass` you can:
    
        $ echo date | sshpass -p password ssh user@host bash
        
    But with `passh` you have to:
    
        $ passh -p password bash -c 'echo date | ssh user@host bash'
        
1. Start SSH SOCKS proxy in background

        $ passh -n -p password ssh -D 7070 -N -n -f user@host
    
    Here `-n` is required or `ssh -f` would not work. (I believe the bug is in OpenSSH though.)
    
1. Login to a remote server

        $ passh -p password user@host
    
1. Run a command on remote server

        $ passh -p password ssh user@host date
        
1. Share a remote server with others and want to use your local `bashrc`?

        $ passh -p password scp /local/bashrc user@host:/tmp/tmp.cAE8Kv
        $ passh -p password ssh -t user@host bash --rc /tmp/tmp.cAE8Kv
        
1. Or just for fun

        $ passh bash
        $ passh vim
