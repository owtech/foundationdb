if [ $1 -eq 1 ]; then
    if [ ! -f /etc/foundationdb/fdb.cluster ]; then
      description=$(LC_CTYPE=C tr -dc A-Za-z0-9 < /dev/urandom | head -c 8)
      random_str=$(LC_CTYPE=C tr -dc A-Za-z0-9 < /dev/urandom | head -c 8)
      echo $description:$random_str@127.0.0.1:4500 > /etc/foundationdb/fdb.cluster
      chown -R foundationdb:foundationdb /etc/foundationdb/
      chmod 0664 /etc/foundationdb/fdb.cluster
      NEWDB=1
    fi

    /usr/bin/systemctl enable foundationdb >/dev/null 2>&1
    /usr/bin/systemctl start foundationdb >/dev/null 2>&1

    if [ "$NEWDB" != "" ]; then
        /usr/bin/fdbcli -C /etc/foundationdb/fdb.cluster --exec "configure new single memory; status" --timeout 20
    fi
else
    DORESTART=1

    if test -f /etc/foundationdb/owtech.conf; then
        value=`grep -E "^RestartWhenUpdate" /etc/foundationdb/owtech.conf | awk -F "=" '{ print $2 }' | tr -d " " | tr "a-z" "A-Z"`
        test "$value" = "NO" -o "$value" = "FALSE" -o "$value" = "0" && DORESTART=0
    fi

    if test $DORESTART -eq 1; then
        systemctl --system daemon-reload > /dev/null || true
        systemctl condrestart foundationdb.service > /dev/null || true
    fi
fi
exit 0

