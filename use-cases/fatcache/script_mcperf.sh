#sync && echo 3 > /proc/sys/vm/drop_caches

DEVICE="vssda"
IFS=

PORT=11211
for count in 1 2 3 4 5
do

	echo ""
	echo "-------------------------------- SET $count --------------------------------"
	echo ""

	./fatcache-0.1.1_vssd/src/fatcache -D /dev/$DEVICE -p $PORT &
	# ./fatcache-0.1.1_original/src/fatcache -D /dev/$DEVICE &

	sleep 5
	PID=$(ps aux|awk '/fatcache-0.1.1/ {print $2}'|awk 'NR==1 {print}')
	echo "Process ID: $PID"

	STATUS="$(cat /sys/block/$DEVICE/stat)"
	START_READ_SECTORS=$(echo $STATUS | awk '{print $3}')
	START_WRITE_SECTORS=$(echo $STATUS | awk '{print $7}')
	START_TIME="$(date +%s)"

	MCPERF_RESULT="$(./mcperf/mcperf-0.1.1/src/mcperf --sizes=u100000,100000 --num-calls=1000  --num-conns=100 \
	--call-rate=1000 --conn-rate=10000 --method=set --server=127.0.0.1 --port=$PORT 2>&1)"
	# echo "McperfResult : $MCPERF_RESULT"


	END_TIME="$(date +%s)"
	STATUS="$(cat /sys/block/$DEVICE/stat)"
	END_READ_SECTORS=$(echo $STATUS | awk '{print $3}')
	END_WRITE_SECTORS=$(echo $STATUS | awk '{print $7}')
	READ=$(($END_READ_SECTORS - $START_READ_SECTORS))
	WRITE=$(($END_WRITE_SECTORS - $START_WRITE_SECTORS))
	DURATION=$(($END_TIME-$START_TIME))
	RPS=$(echo $MCPERF_RESULT | awk '/rsp/ {print $3}')
	STORED=$(echo $MCPERF_RESULT | awk '/stored/ {print $4}')

	echo "End time: ${END_TIME}. Start time: ${START_TIME}. Duration: ${DURATION} seconds"
	echo "Data read: ${READ} sectors = $((READ*512)) Bytes = $((READ/2048)) MB"
	echo "Data written: ${WRITE} sectors = $((WRITE*512)) Bytes = $((WRITE/2048)) MB"
	echo "Read: $((READ/(2048*DURATION))) MB/s, Write: $((WRITE/(2048*DURATION))) MB/s. Aggregate: $(((READ+WRITE)/(2048*DURATION))) MB/s"
	echo "Set Rps: $RPS. Stored: $STORED"
	echo -e "$((READ/2048))\t$((WRITE/2048))\t${DURATION}\t$(((READ+WRITE)/(2048*DURATION)))\t$RPS\t$STORED"


	echo ""
	echo "-------------------------------- GET $count --------------------------------"
	echo ""
	STATUS="$(cat /sys/block/$DEVICE/stat)"
	START_READ_SECTORS=$(echo $STATUS | awk '{print $3}')
	START_WRITE_SECTORS=$(echo $STATUS | awk '{print $7}')
	START_TIME="$(date +%s)"

	MCPERF_RESULT="$(./mcperf/mcperf-0.1.1/src/mcperf --sizes=u100000,100000 --num-calls=1000  --num-conns=100 \
	--call-rate=50 --conn-rate=10000 --method=get --server=127.0.0.1 --port=$PORT 2>&1)"
	# echo "McperfResult : $MCPERF_RESULT"

	END_TIME="$(date +%s)"
	STATUS="$(cat /sys/block/$DEVICE/stat)"
	END_READ_SECTORS=$(echo $STATUS | awk '{print $3}')
	END_WRITE_SECTORS=$(echo $STATUS | awk '{print $7}')
	READ=$(($END_READ_SECTORS - $START_READ_SECTORS))
	WRITE=$(($END_WRITE_SECTORS - $START_WRITE_SECTORS))
	DURATION=$(($END_TIME-$START_TIME))
	RPS=$(echo $MCPERF_RESULT | awk '/rsp/ {print $3}')
	HIT=$(echo $MCPERF_RESULT | awk '/value/ {print $10}')
	MISS=$(echo $MCPERF_RESULT | awk '/end/ {print $8}')


	echo "End time: ${END_TIME}. Start time: ${START_TIME}. Duration: ${DURATION} seconds"
	echo "Data read: ${READ} sectors = $((READ*512)) Bytes = $((READ/2048)) MB"
	echo "Data written: ${WRITE} sectors = $((WRITE*512)) Bytes = $((WRITE/2048)) MB"
	echo "Read: $((READ/(2048*DURATION))) MB/s, Write: $((WRITE/(2048*DURATION))) MB/s. Aggregate: $(((READ+WRITE)/(2048*DURATION))) MB/s"
	echo "Get Rps: $RPS. Hit: $HIT. Miss: $MISS. Total: $((HIT + MISS))"
	echo -e "$((READ/2048))\t$((WRITE/2048))\t${DURATION}\t$(((READ+WRITE)/(2048*DURATION)))\t$RPS\t$HIT\t$MISS\t$((HIT + MISS))"

	kill -2 $PID
	sleep 10

	# PORT=$((PORT+1))

done

