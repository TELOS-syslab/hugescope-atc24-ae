./bin/ycsb run redis -s -threads 32 -P workloads/workloada2 -p "redis.host=127.0.0.1" -p "redis.port=6379" > outputRedisRun.txt
./bin/ycsb run redis -s -threads 32 -P workloads/workloada4 -p "redis.host=127.0.0.1" -p "redis.port=6379" >> outputRedisRun.txt
./bin/ycsb run redis -s -threads 32 -P workloads/workloada6 -p "redis.host=127.0.0.1" -p "redis.port=6379" >> outputRedisRun.txt
