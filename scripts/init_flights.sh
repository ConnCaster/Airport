NOW=$(date +%s)

curl -s -X POST http://localhost:8082/v1/flights/init \
  -H "Content-Type: application/json" \
  -d "{
    \"flights\": [
      {\"flightId\": \"SU100\", \"scheduledAt\": $NOW, \"status\": \"Scheduled\"},
      {\"flightId\": \"SU101\", \"scheduledAt\": $NOW, \"status\": \"Scheduled\"}
    ]
  }"

