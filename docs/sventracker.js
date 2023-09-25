var database_server = "https://w00tguy.no-ip.org:5555/";
var g_server_data = null;
var g_rankings = null;
var g_server_list = [];
var g_server_info = {};

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
			loader.innerHTML = "Failed to load file: " + path + "<br><br>" + e;
		}
	});
}

function init_server_info(key) {
	g_server_info[key] = {
		rank: 0,
		name: '<div class="unresponsive">server not responding</div>',
		players: '<div class="unresponsive">OFFLINE</div>',
		addr: key.replace("_", ":"),
		map: '<div class="unresponsive">OFFLINE</div>',
	};
}

function init_page() {
	var loader = document.getElementsByClassName("site-loader")[0];
	loader.classList.remove("loader");
	update_table();
}

function update_table() {
	var table = document.getElementsByClassName("server-table")[0];
	var row_template = document.getElementsByClassName("row-template")[0];
	
	var rank_age = 0;
	var rank_freq = 0;
	var rank_idx = 1;
	g_servers = [];
	
	var oldRows = document.getElementsByClassName("server-row");
	while(oldRows[0]) {
		oldRows[0].parentNode.removeChild(oldRows[0]);
	}
	
	for (var i = 0; i < g_rankings.length; i++) {
		parts = g_rankings[i].split("=");
		if (parts.length < 3) {
			continue;
		}
		var name = parts[2];
		for (var k = 3; k < parts.length; k++) {
			name += "=" + parts[k]; // in case server names have equals signs in them
		}
		if (name.length == 0) {
			continue;
		}
		
		if (parts[0] == "rank_age") {
			rank_age = parseInt(parts[1]);
			continue;
		}
		if (parts[0] == "rank_freq") {
			rank_freq = parseInt(parts[1]);
			continue;
		}
		var rank = parseInt(parts[0]);
		var serverId = parts[1];
		
		g_servers.push(serverId);
		
		if (!(serverId in g_server_info)) {
			init_server_info(serverId);
		} 
		
		g_server_info[serverId]["rank"] = rank;
		
		var row = row_template.cloneNode(true);
		row.setAttribute("class", "server-row " + serverId);
		row.getElementsByClassName("rank-cell")[0].innerHTML = rank_idx;
		row.getElementsByClassName("name-cell")[0].innerHTML = "<a>" + (name || g_server_info[serverId]["name"]) + "</a>";
		row.getElementsByClassName("addr-cell")[0].innerHTML = g_server_info[serverId]["addr"];
		row.getElementsByClassName("players-cell")[0].innerHTML = g_server_info[serverId]["players"];
		row.getElementsByClassName("map-cell")[0].innerHTML = g_server_info[serverId]["map"];
		table.appendChild(row);
		rank_idx += 1;
	}
}

function update_server_info() {	
	for (var i = 0; i < g_server_data.length; i++) {
		var id = g_server_data[i]["addr"].replace(":", "_");
		
		if (!(id in g_server_info)) {
			init_server_info(id);
		}
		
		g_server_info[id]["name"] = g_server_data[i]["name"];
		g_server_info[id]["players"] = g_server_data[i]["players"] + " / " + g_server_data[i]["max_players"];
		g_server_info[id]["addr"] = g_server_data[i]["addr"];
		g_server_info[id]["map"] = g_server_data[i]["map"];
	}
	
	update_table();
}

function load_server_json() {
	console.log("Fetch servers.json");
	fetchJSONFile(database_server + "servers.json", function(data) {
		console.log("Server data: ", data);
		g_server_data = data;
		update_server_info();
	});
}

function load_server_rankings() {
	console.log("Fetch ranking data");
	fetchTextFile(database_server + "rankings.txt", function(data) {
		console.log("Ranking data:\n", data);
		g_rankings = data.match(/[^\r\n]+/g);
		init_page();
	});
}

var g_should_refresh_servers = false;
var g_should_refresh_rankings = false;

function load_server_list() {
	load_server_rankings();
	setInterval(function () {
		if (!document.hidden) {
			load_server_rankings();
		} else {
			g_should_refresh_rankings = true;
			console.log("Tab not active. Not fetching.");
		}
	}, 1000*60*60);
	
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
			if (g_should_refresh_rankings) {
				g_should_refresh_rankings = false;
				load_server_rankings();
			}
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