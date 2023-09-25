var database_server = "https://w00tguy.no-ip.org:5555/";
var g_server_data = null;
var g_rankings = null;
var g_server_list = [];

function fetchTextFile(path, callback) {
	var httpRequest = new XMLHttpRequest();
	httpRequest.onreadystatechange = function() {
		if (httpRequest.readyState === 4 && httpRequest.status === 200 && callback) {
			callback(httpRequest.responseText);
		}
	};
	httpRequest.open('GET', path + '?nocache=' + (new Date()).getTime());
	httpRequest.send();
}

function fetchBinaryFile(path, callback) {
	var httpRequest = new XMLHttpRequest();
	httpRequest.onreadystatechange = function() {
		if (httpRequest.readyState === 4 && httpRequest.status === 200 && callback) {
			callback(httpRequest.response);
		} else if (httpRequest.readyState === 4 && callback) {
			callback(null);
		}
	};
	httpRequest.open('GET', path);
	httpRequest.responseType = "blob";
	httpRequest.send();
}

function fetchJSONFile(path, callback) {	
	fetchTextFile(path, function(data) {
		try {
			callback(JSON.parse(data));
		} catch(e) {
			console.error("Failed to load JSON file: " + path +"\n\n", e);
			
			var loader = document.getElementsByClassName("site-loader")[0];
			loader.classList.remove("loader");
			loader.innerHTML = "Failed to load file: " + path + "<br><br>" + e;
		}
	});
}

function rankCompare(a, b) {
	let rankA = g_server_data["servers"][a]["rank"];
	let rankB = g_server_data["servers"][b]["rank"];
	
	if (rankA > rankB) {
		return -1;
	} else if (rankB > rankA) {
		return 1;
	}
	
	return 0;
}

function update_table() {
	var servers = g_server_data["servers"];
	var table = document.getElementsByClassName("server-table")[0];
	var row_template = document.getElementsByClassName("row-template")[0];
	
	var oldRows = document.getElementsByClassName("server-row");
	while(oldRows[0]) {
		oldRows[0].parentNode.removeChild(oldRows[0]);
	}
	
	g_server_list = [];
	for (let key in servers) {
		g_server_list.push(key);
	}
	g_server_list.sort(rankCompare);
	
	var updateTime = g_server_data["lastUpdateTime"];
	
	for (var i = 0; i < g_server_list.length; i++) {
		var key = g_server_list[i];
		
		var offlineTime = Math.round((updateTime - servers[key]["time"]) / 60);
		
		var row = row_template.cloneNode(true);
		row.setAttribute("class", "server-row " + key);
		row.getElementsByClassName("rank-cell")[0].innerHTML = i+1;
		row.getElementsByClassName("name-cell")[0].innerHTML = "<a>" + (name || servers[key]["name"]) + "</a>";
		row.getElementsByClassName("addr-cell")[0].innerHTML = key.replace("_", ":");
		
		if (offlineTime < g_server_data["unreachableTime"]/60) {
			row.getElementsByClassName("players-cell")[0].innerHTML = servers[key]["players"] + " / " + servers[key]["max_players"];
			row.getElementsByClassName("map-cell")[0].innerHTML = servers[key]["map"];
		} else {
			var tooltip = offlineTime + " minutes since the last server response";
			var offline = '<div class="unresponsive" title="' + tooltip + '">OFFLINE</div>';;
			row.getElementsByClassName("players-cell")[0].innerHTML = offline
			row.getElementsByClassName("map-cell")[0].innerHTML = offline;
		}
		
		table.appendChild(row);
	}
	
	var loader = document.getElementsByClassName("site-loader")[0];
	loader.classList.remove("loader");
}

function load_server_json() {
	console.log("Fetch tracker data");
	fetchJSONFile(database_server + "tracker.json", function(data) {
		console.log("Tracker data: ", data);
		g_server_data = data;
		update_table();
	});
}

var g_should_refresh_servers = false;

function load_server_list() {	
	load_server_json();
	
	setInterval(function () {
		if (!document.hidden) {
			load_server_json();
		} else {
			g_should_refresh_servers = true;
			console.log("Tab not active. Not fetching.");
		}
	}, 1000*60);
	
	setInterval(function () {
		if (!document.hidden) {
			if (g_should_refresh_servers) {
				g_should_refresh_servers = false;
				load_server_json();
			}
		}
	}, 1000);
}

document.addEventListener("DOMContentLoaded",function() {
	load_server_list();	
});