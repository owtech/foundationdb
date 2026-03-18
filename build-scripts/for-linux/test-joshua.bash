#!/bin/bash

# $1 - the path to the correctness archive file, ex. correctness-7.3.49-2.ow.tar.gz
# $2 - number of tests

python3 -m joshua.joshua start --tarball $1 --max-runs $2

python3 -m joshua.joshua tail | python3 -c "
import sys, re
sys.stdout = open(sys.stdout.fileno(), 'w', buffering=1) 
from datetime import datetime
failed = False
count = 1
for line in sys.stdin:
    m = re.search(r'TestFile=\"([^\"]+)\".*?Ok=\"(\d+)\"', line)
    if m:
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f'[#{count} {timestamp}] TestFile=\"{m.group(1)}\" Ok=\"{m.group(2)}\"')
        if m.group(2) == '0':
            failed = True
    else:
        sys.stdout.write(line)
    count += 1
sys.exit(1 if failed else 0)
" &

TAIL_PID=$!

while kill -0 "$TAIL_PID" 2>/dev/null; do
    sleep 30
    if ! podman ps -q 2>/dev/null | grep -q .; then
        echo "ERROR: all joshua-agent containers have stopped unexpectedly" >&2
        kill "$TAIL_PID" 2>/dev/null
        echo "Stopping joshua due to container failure..."
        python3 -m joshua.joshua stop
        exit 1
    fi
done

wait "$TAIL_PID"
exit $?
