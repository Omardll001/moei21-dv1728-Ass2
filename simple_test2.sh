#!/bin/bash
# Simplified Test2 to verify the fix

server=localhost
serverport=5352
tests=5
drop_prob=0

echo "Date:" $(date)
echo "Starting your server on $server:$serverport"
./udpserver ${server}:$serverport > server.log 2>server_err.log &
SERVER_PID=$!    
echo "Sleep abit."
sleep 2

echo "server on " $(lsof -i:$serverport  | grep udpserver)

serverStatus=$(lsof -i:$serverport | grep udpserver)
if [[ $serverStatus == *"IPv4"* ]]; then
    echo "Looks good, you got a IPv4 Socket"
else
    echo "Strange, you should have a IPv4 Socket for your server."
    echo "You have $serverStatus ."
    echo "I'll kill the server, and print any log it generated."
    kill $SERVER_PID
    killall udpserver
    sleep 1
    echo "This was the log"
    cat server.log
    echo "<end of log>"
    exit 1
fi

echo "PID = $SERVER_PID" 
echo "Checking server.log"
cat server.log 

echo "Running simple bulk test with $tests tests, $drop_prob% drop rate"
rm -rf server_test.log
timeout 30s ./bulkUDPclient ${server}:$serverport $tests $drop_prob server_test.log >client.log &
CLIENT_PID=$!

echo "Waiting for test to complete, max 30s"
wait $CLIENT_PID
CLIENT_EXIT_CODE=$?

echo "Killing server"
kill $SERVER_PID 2>/dev/null
killall udpserver 2>/dev/null

echo "Client exit code: $CLIENT_EXIT_CODE"
echo "=== server.log ==="
cat server.log
echo "=== client.log ==="  
cat client.log
echo "=== server_test.log ==="
cat server_test.log 2>/dev/null || echo "(no server_test.log created)"

if [ -f server_test.log ]; then
    result=$(cat client.log | grep 'SUMMARY: PASSED!' || echo "")
    if [ -n "$result" ]; then
        echo "SUCCESS: Test completed successfully!"
        tail -1 client.log
    else
        echo "FAILURE: Test did not pass"
    fi
else
    echo "FAILURE: No server_test.log created"
fi