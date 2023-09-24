import urllib.request
from pathlib import Path
import json, time, os

api_key = open("api_key.txt", "r").read().strip()
sever = "https://api.steampowered.com"
api = "/IGameServersService/GetServerList/v1"
stats_path = "stats"

game_id = "225840" # Sven Co-op
filter = "\\appid\\" + game_id + "\\dedicated\\1"
limit = 5000

query_timeout = 10
write_frequency = 60 # time between stat writes

Path(stats_path).mkdir(parents=True, exist_ok=True)

server_state_template = {
	"name": "",
	"maxplayers": 0,
	"players": 0,
	"map": "",
	"last_stat": 0
}

g_servers = {}

while True:
	try:
		print("Query server list")
		f = urllib.request.urlopen(sever + api + "?key=" + api_key + "&filter=" + filter + "&limit=%s" % limit, timeout=query_timeout)
	except Exception as e:
		print(e)
		time.sleep(query_timeout*2)
		continue
		
	server_json = json.loads(f.read().decode('ascii', 'ignore'))
	os.system("cls")
	
	if "servers" not in server_json["response"]:
		print("Failed to get server list")
		time.sleep(query_timeout)
		continue
	
	servers = server_json["response"]["servers"]
	servers = sorted(servers, key=lambda k: k['players']) 

	for server in servers:
		#print(server)
		addr = server['addr']
		
		if addr not in g_servers:
			g_servers[addr] = server_state_template
			
		keyframe_needed = g_servers[addr]
	
		if server['players'] > 1 and server['dedicated']:
			print("%3d / %-2d  %s " % (server['players'], server['max_players'], server['name']))
		
	time.sleep(30)
