All the examples are tested with `sshpass-1.06`.

Example #1
----------

With `sshpass`:

    $ tty
    /dev/pts/18                                      // now we're on pts/18
    $ sshpass bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device    <<< Bash not happy
    bash: no job control in this shell                                              <<< Bash not happy
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

Example #2
----------

    $ sshpass bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell
    bash-5.0#    <== Press Ctrl-C here and the shell will be killed.

Example #3
----------

    $ sshpass bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell
    bash-5.0# sleep 60
        <== Press Ctrl-C to kill 'sleep' but Bash would also be killed.

Example #4
----------

    $ sshpass bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell
    bash-5.0# read < /dev/tty
    it
    cannot
    read
    from
    /dev/tty
              <== The 'read' never returns. Ctrl-D does not work either.
              <== You have to press Ctrl-C to kill bash.

Example #5
----------

    $ sshpass bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell
    bash-5.0# echo password: > /dev/tty
    bash-5.0#                            <== It hangs here. Press Ctrl-C to kill it.

Example #6
----------

    $ sshpass -p xxx bash --norc
    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell
    bash-5.0# echo password: > /dev/tty
    bash-5.0# echo password: > /dev/tty
    $                                    <== The two echo commands killed Bash.

Example #7
----------

    # sshpass -p some-passwd ssh no-such-user@127.0.0.1 date
    # echo $?                            <== It silently fails. No error messages.
    5
    #
