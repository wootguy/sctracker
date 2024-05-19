var database_server = "https://w00tguy.no-ip.org/hltracker/";
var stats_live_path = "stats/live/";
var stats_avg_path = "stats/avg/";
var g_server_data = null;
var g_server_list = [];
var g_server_stats = {};
var g_data_cache = {};

const FL_PCNT_TIME16 = 64;		// time delta is 16 bits and relative to the last stat
const FL_PCNT_TIME32 = 128;		// time is a 32 bit absoulte value
const PCNT_FL_MASK = (FL_PCNT_TIME16|FL_PCNT_TIME32);
const PCNT_UNREACHABLE = (PCNT_FL_MASK);

const RANK_STAT_INTERVAL = 60; // gaps between rank data points
const RANK_STAT_AVG_INTERVAL = 60*60; // gaps between rank data points in averaged data
var g_timeWindow = 60*60*24*7; // ignore stats older than this when generating graphs
var g_useAvgData = false;

var g_should_refresh_servers = false;
var auto_refresh = true;

const FL_SERVER_DEDICATED = 1;
const FL_SERVER_SECURE = 2;
const FL_SERVER_LINUX = 4;

var refreshInterval;
var jsonInterval;

var g_graphLineColor = "#d97400";
var g_serverLimit = 1000;

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
			var fileReader = new FileReader();

			fileReader.onload = function (event) {
				if (event.target.readyState === FileReader.DONE) {
					// The ArrayBuffer containing the Blob's data is available in event.target.result
					var arrayBuffer = event.target.result;

					// Pass the ArrayBuffer to the callback
					callback(arrayBuffer);
				}
			};
			
			fileReader.readAsArrayBuffer(httpRequest.response);
		} else if (httpRequest.readyState === 4 && callback) {
			callback(null);
		}
	};
	httpRequest.open('GET', path + '?nocache=' + (new Date()).getTime());
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

function renderGraph(serverid) {	
	let datapoints = g_server_stats[serverid]["data"];
	
	var row = document.getElementsByClassName("server-content-row " + serverid)[0];
	var chart = row.getElementsByClassName("chart")[0];
	
	var chartg = row.getElementsByClassName("chartg")[0];
	var chartWidthPad = 30;
	var chartHeightPad = 10;
	var svgWidth = row.offsetWidth - 20;
	var chartWidth = svgWidth - (chartWidthPad);
	var svgHeight = chart.getAttribute("height");
	var chartHeight = svgHeight - (chartHeightPad*2);
	
	chart.setAttribute("width", svgWidth + "px");
	chart.setAttribute("viewBox", "0 " + -svgHeight + " " + svgWidth + " " + svgHeight);
	
	var yScale = chartHeight / 32.0;
	var xScale = chartWidth / (datapoints.length);
	
	// horizontal lines
	chartg.innerHTML = "";
	for (let i = 0; i <= 32; i+= 8) {
		var start = chartWidthPad-5 + "," + (i*yScale + chartHeightPad);
		var end = (chartWidth+chartWidthPad) + "," + (i*yScale + chartHeightPad);
		var barpoints = start + " " + end;
		var polyline = '<polyline fill="none" stroke="#444" stroke-width="1" points="' + barpoints + '"/>';
		chartg.innerHTML += polyline;
		
		chartg.innerHTML += '<text x="' + (chartWidthPad-10) + '" y="' + -(i*yScale + chartHeightPad) + '" fill="#ddd" transform="scale(1,-1)">' + i + '</text>';
	}
	
	// vertical borders
	{
		var start = chartWidthPad + "," + chartHeightPad;
		var end = chartWidthPad + "," + (chartHeightPad+chartHeight);
		var polyline = '<polyline fill="none" stroke="#444" stroke-width="1" points="' + start + " " + end + '"/>';
		chartg.innerHTML += polyline;
		
		var start = (chartWidth+chartWidthPad-1) + "," + chartHeightPad;
		var end = (chartWidth+chartWidthPad-1) + "," + (chartHeightPad+chartHeight);
		var polyline = '<polyline fill="none" stroke="#444" stroke-width="1" points="' + start + " " + end + '"/>';
		chartg.innerHTML += polyline;
	}
	
	let unreachablePeriods = [];
	let unreachableStart = -1;
	let unreachableWasProgramRestart = false;
	
	// data points
	var maxValue = 32;
	var points = "";
	for (let i = 0; i < datapoints.length; i++) {
		let players = datapoints[i];
		if (datapoints[i] == -3) {
			continue; // no data yet
		}
		else if (datapoints[i] < 0) {
			players = 0;
			if (unreachableStart == -1) {
				unreachableStart = i;
				unreachableWasProgramRestart = datapoints[i] == -2;
			}
		}
		if (unreachableStart != -1 && (datapoints[i] >= 0 || i == datapoints.length-1)) {
			let xStart = chartWidthPad + unreachableStart*xScale;
			let rectW = (chartWidthPad + (i-1)*xScale) - xStart;
			let rect = '<rect x="' + xStart + '" y="' + chartHeightPad + '" width="' + rectW 
						+ '" height="' + chartHeight;
						
			if (unreachableWasProgramRestart) {
				rect += '" fill="#444"><title>The stat collector was not running for some or all of this time.</title></rect>';
			} else {
				rect += '" fill="#600"><title>The server was unreachable during this time.</title></rect>';
			}
			
			unreachablePeriods.push(rect);
			chartg.innerHTML += rect;
			unreachableStart = -1;
		}
		
		points += " " + (chartWidthPad + i*xScale) + "," + (chartHeightPad + players * yScale);
	}
	{
		var polyline = '<polyline fill="none" stroke="' + g_graphLineColor + '" stroke-width="1" points="' + points + '"/>';
		chartg.innerHTML += polyline;
	}
	
	console.log("plot " + datapoints.length + " points (" + g_server_stats[serverid]["dataView"].byteLength + " bytes)");
}

function parseStatFile(serverid, dataView) {	
	let offset = 0;
	
	const version = dataView.getUint32(offset, true);
	offset += 4;
	
	const magicBytes = [];
	for (let i = 0; i < 4; i++) {
		magicBytes.push(dataView.getUint8(offset, true));
		offset++;
	}
	let magic = new TextDecoder('utf-8').decode(new Uint8Array(magicBytes));
	
	if (version != 1) {
		console.error("Invalid stat file version: " + version + " != " + 1);
		return;
	}
	
	if (magic != "SVTK") {
		console.error("Invalid stat file magic bytes: " + magic + " != SVTK");
		return;
	}
	
	let now = Math.round(new Date().getTime() / 1000);
	let rankDataPoints = [];
	let playerCount = 0;
	let rankStartTime = now - g_timeWindow;
	let nextRankTime = rankStartTime;
	let statTime = 0;
	let lastStatTime = 0;
	let lastUnreachable = false;
	let lastUnreachableWasProgramRestart = false;
	let historyStarted = false;
	let interval = g_useAvgData ? RANK_STAT_AVG_INTERVAL : RANK_STAT_INTERVAL;
	let expectedRankDatapoints = ((g_timeWindow) / (interval));
	
	while (offset < dataView.byteLength) {
		let stat = dataView.getUint8(offset, true);
		offset += 1;
		
		let flags = stat & PCNT_FL_MASK;
		let newPlayerCount = 0;
		let unreachable = false;
		
		if ((stat & PCNT_FL_MASK) == PCNT_UNREACHABLE) {
			newPlayerCount = 0;
			unreachable = true;
			flags = (stat << 2) & PCNT_FL_MASK;
			if (stat & 0x0f) {
				console.log("Invalid flags in unreachable byte " + stat);
			}
		}
		else {
			newPlayerCount = stat & ~PCNT_FL_MASK;
			if (newPlayerCount > 32) {
				console.log("Invalid player count");
			}
		}

		if (flags & FL_PCNT_TIME32) {
			let newTime = dataView.getUint32(offset, true);
			offset += 4;
			statTime = newTime;
		}
		else if (flags & FL_PCNT_TIME16) {
			let delta = dataView.getUint16(offset, true);
			offset += 2;
			statTime += delta;
		}
		else {
			let delta = dataView.getUint8(offset, true);
			offset += 1;
			statTime += delta;
		}

		while (statTime >= nextRankTime) { // back-fill gaps in data with last known player count
			if (!historyStarted) {
				rankDataPoints.push(-3);
			} else if (lastUnreachable && lastUnreachableWasProgramRestart) {
				rankDataPoints.push(-2);
			} else if (lastUnreachable) {
				rankDataPoints.push(-1);
			} else {
				rankDataPoints.push(playerCount);
			}
			
			nextRankTime = rankStartTime + rankDataPoints.length * interval;
		}

		playerCount = newPlayerCount;
		lastUnreachable = unreachable;
		if (unreachable) {
			// program writes an unreachable stat at the same time as the last data point when restarted
			lastUnreachableWasProgramRestart = lastStatTime == statTime;
		}
		lastStatTime = statTime;
		historyStarted = true;
		
	}
	
	while (now >= nextRankTime) { // back-fill gaps in data with last known player count
		if (lastUnreachable && lastUnreachableWasProgramRestart) {
			rankDataPoints.push(-2);
		} else if (lastUnreachable) {
			rankDataPoints.push(-1);
		} else {
			rankDataPoints.push(playerCount);
		}
		
		nextRankTime = rankStartTime + rankDataPoints.length * interval;
	}
	rankDataPoints.pop();
	if (rankDataPoints.length != expectedRankDatapoints) {
		console.log("Unexpected rank data points" + rankDataPoints.length + "/" + expectedRankDatapoints);
		rankDataPoints = [];
	}
	
	g_server_stats[serverid] = {
		data: rankDataPoints,
		dataView: dataView
	}
}

function fetch_graph(serverid) {
	let datpath = database_server + (g_useAvgData ? stats_avg_path : stats_live_path) + serverid + ".dat";
	
	if (datpath in g_data_cache) {
		console.log("Use cached: " + datpath);
		parseStatFile(serverid, g_data_cache[datpath]);
		renderGraph(serverid);
	} else {
		console.log("Fetch: " + datpath);
		fetchBinaryFile(datpath, function(data) {
			if (data) {
				g_data_cache[datpath] = new DataView(data);
				parseStatFile(serverid, g_data_cache[datpath]);
				renderGraph(serverid);
			} else {
				console.error("Failed to fetch graph data");
			}
		});
	}	
}

function expand_server_row(serverid, redraw) {
	var expand_content = document.getElementsByClassName("server-content-row " + serverid)[0];
	var expand_row = document.getElementsByClassName("server-row " + serverid)[0];
	
	expand_content.classList.add("expanded");
	expand_row.classList.add("expanded");
	
	if (redraw) {
		if (serverid in g_server_stats) {
			parseStatFile(serverid, g_server_stats[serverid]["dataView"]);
			renderGraph(serverid);
		} else {
			fetch_graph(serverid);
		}
	}
}

function update_table() {
	var servers = g_server_data["servers"];
	var table_body = document.getElementsByClassName("server-table-body")[0];
	var row_template = document.getElementsByClassName("row-template")[0];
	var expand_content_template = document.getElementsByClassName("server-content-row-template")[0];
	var showOfflineServers = document.getElementById("filter_offline").checked;
	var showDeadServers = document.getElementById("filter_dead").checked;
	var hideCollapsedServers = document.getElementById("filter_collapsed").checked;
	
	var reload_graph_keys = [];
	
	var contentDiv = document.getElementsByClassName("content-container")[0];
	var oldScrollPos = contentDiv.scrollTop;
	
	var oldRows = document.getElementsByClassName("server-content-row");
	for (var i = 0; i < oldRows.length; i++) {
		if (oldRows[i].classList.contains("expanded")) {
			reload_graph_keys.push(oldRows[i].getAttribute("key"));
		}
	}
	
	if (reload_graph_keys.length == 0) {
		document.getElementById("filter_collapsed").disabled = true;
		hideCollapsedServers = false;
	} else {
		document.getElementById("filter_collapsed").disabled = false;
	}
	
	table_body.textContent = "";
	
	g_server_list = [];
	for (let key in servers) {
		g_server_list.push(key);
	}
	g_server_list.sort(rankCompare);
	
	var updateTime = g_server_data["lastUpdateTime"];
	
	var addedRows = 0;
	
	for (var i = 0; i < g_server_list.length && i < g_serverLimit; i++) {
		var key = g_server_list[i];
		
		var offlineTime = Math.round((updateTime - servers[key]["time"]) / 60);
		var playerHours = servers[key]["rank"] / 60;
		
		if (!showOfflineServers && offlineTime > 0) {
			continue;
		}
		
		if (!showDeadServers && playerHours < 14) {
			continue;
		}
		
		if (hideCollapsedServers && reload_graph_keys.indexOf(key) == -1) {
			addedRows++;
			continue;
		}
		
		var row = row_template.cloneNode(true);
		var classodd = addedRows % 2 ? "odd" : "even";
		var gameClass = document.getElementById("game_selector").value;
		row.setAttribute("class", "row server-row " + key + " " + classodd + " " + gameClass);
		row.setAttribute("serverid", key);
		row.getElementsByClassName("rank-cell")[0].textContent = addedRows+1;
		row.getElementsByClassName("rank-cell")[0].title = playerHours.toLocaleString(undefined, { maximumFractionDigits: 0  }) + " player hours";
		row.getElementsByClassName("name-cell")[0].textContent = servers[key]["name"];
		row.getElementsByClassName("addr-cell")[0].textContent = key.replace("_", ":");
		
		if (offlineTime < g_server_data["unreachableTime"]/60) {
			row.getElementsByClassName("players-cell")[0].textContent = servers[key]["players"] + " / " + servers[key]["max_players"];
			row.getElementsByClassName("map-cell")[0].textContent = servers[key]["map"];
		} else {
			var tooltip = offlineTime + " minutes since the last server response";
			var offline = '<div class="unresponsive" title="' + tooltip + '">OFFLINE</div>';
			row.getElementsByClassName("players-cell")[0].innerHTML = offline
			row.getElementsByClassName("map-cell")[0].innerHTML = offline;
		}
		
		row.addEventListener("click", function() {
			var serverid = event.currentTarget.getAttribute("serverid");
			var expand_content = document.getElementsByClassName("server-content-row " + serverid)[0];
			
			if (expand_content.classList.contains("expanded")) {
				expand_content.classList.remove("expanded");
				event.currentTarget.classList.remove("expanded");
			} else {
				if (document.getElementsByClassName("server-content-row expanded").length >= 50) {
					alert("50 graphs max. Close the other ones.");
				} else {
					document.getElementById("filter_collapsed").disabled = false;
					expand_server_row(serverid, true);
				}
			}
		});
		
		table_body.appendChild(row);
		
		var content = expand_content_template.cloneNode(true);
		content.setAttribute("class", "server-content-row " + key + " " + classodd);
		content.setAttribute("key", key);
		table_body.appendChild(content);
		
		addedRows++;
	}
	
	console.log("Table updated");
	
	for (var i = 0; i < reload_graph_keys.length; i++) {
		expand_server_row(reload_graph_keys[i], true);
	}
	refetch_charts(1000);
	
	var loader = document.getElementsByClassName("site-loader")[0];
	loader.classList.remove("loader");
	
	var now = Math.round(new Date().getTime() / 1000);
	var timeLeft = g_server_data["rankFreq"] - (now - g_server_data["lastRankTime"]);
	console.log("Next rank update in " + Math.round(timeLeft/60) + " minutes");
	
	contentDiv.scrollTop = oldScrollPos;
}

function load_server_json() {
	console.log("Fetch tracker data");
	fetchJSONFile(database_server + "tracker.json", function(data) {
		//console.log("Tracker data: ", data);
		console.log("Tracker data loaded");
		g_server_data = data;
		g_data_cache = {};
		update_table();
	});
}

function load_server_list() {
	load_server_json();
	
	clearTimeout(refreshInterval);
	clearTimeout(jsonInterval);
	
	refreshInterval = setInterval(function () {
		if (auto_refresh) {
			if (!document.hidden) {
				load_server_json();
			} else {
				g_should_refresh_servers = true;
				console.log("Tab not active. Not fetching.");
			}
		}
	}, 1000*60);
	
	jsonInterval = setInterval(function () {
		if (!document.hidden && auto_refresh) {
			if (g_should_refresh_servers) {
				g_should_refresh_servers = false;
				load_server_json();
			}
		}
	}, 1000);
}

window.onresize = handle_resize;

function handle_resize(event) {	
	var controls = document.getElementsByClassName("fixed-controls")[0];
	var header = document.getElementsByClassName("header")[0];
	var content = document.getElementsByClassName("content-container")[0];
	
	content.style.height = "" + (window.innerHeight - (Math.ceil(controls.offsetHeight))) + "px";
	
	redraw_charts();
};

function refetch_charts(delay) {
	var graphs = document.getElementsByClassName("server-content-row expanded");
	for (var i = 0; i < graphs.length; i++) {
		let serverid = graphs[i].getAttribute("key");
		
		setTimeout(function () {
			fetch_graph(serverid);
		}, delay*i);
	}
}

function reload_charts() {
	var graphs = document.getElementsByClassName("server-content-row expanded");
	for (var i = 0; i < graphs.length; i++) {
		var serverid = graphs[i].getAttribute("key");
		if (g_server_stats[serverid] == undefined) {
			fetch_graph(serverid);
		} else {
			parseStatFile(serverid, g_server_stats[serverid]["dataView"]);
			renderGraph(serverid);
		}
	}
}

function redraw_charts() {
	var charts = document.getElementsByClassName("chart");
	for (var i = 0; i < charts.length; i++) {
		charts[i].setAttribute("width", "0px");
	}
	
	var graphs = document.getElementsByClassName("server-content-row expanded");
	for (var i = 0; i < graphs.length; i++) {
		renderGraph(graphs[i].getAttribute("key"));
	}
}

function change_time_window() {
	var timebuts = document.getElementsByClassName("chart-time");
	for (let i = 0; i < timebuts.length; i++) {
		timebuts[i].classList.remove("active");
	}
	event.currentTarget.classList.add("active");
	let minutes = parseInt(event.currentTarget.getAttribute("minutes"));
	g_timeWindow = minutes*60;
	
	let wasUsingAvg = g_useAvgData;
	g_useAvgData = minutes > 60*24*30;
	
	if (g_useAvgData != wasUsingAvg) {
		g_server_stats = {};
		refetch_charts(0);
	} else {
		reload_charts();
	}
}

function update_game() {
	var game = document.getElementById("game_selector").value;
	
	if (game == "hl") {
		document.getElementById("game-title").textContent = "Half-Life";
		g_graphLineColor = "#d97400";
		
	} else if (game == "sc") {
		document.getElementById("game-title").textContent = "Sven Co-op";
		g_graphLineColor = "#0074d9";
	} else if (game == "rc") {
		document.getElementById("game-title").textContent = "Ricochet";
		g_graphLineColor = "#d90000";
	} else if (game == "cs") {
		document.getElementById("game-title").textContent = "Counter-Strike";
		g_graphLineColor = "#d9d900";
	}
	
	var loader = document.getElementsByClassName("site-loader")[0];
	loader.classList.add("loader");
	
	document.getElementById("filter_collapsed").checked = false;
	
	database_server = "https://w00tguy.no-ip.org/" + game + "tracker/";
	console.log("Using DB server: " + database_server);
	
	var theader = document.getElementsByClassName("server-table-header")[0];
	theader.setAttribute("class", "server-table-header " + game);

	var table_body = document.getElementsByClassName("server-table-body")[0];
	table_body.textContent = "";
	load_server_list();
}

document.addEventListener("DOMContentLoaded",function() {
	update_game();
	
	handle_resize();
	
	var timeControls = document.getElementsByClassName("chart-time");
	for (var i = 0; i < timeControls.length; i++) {
		timeControls[i].addEventListener("click", change_time_window);
	}
	
	document.getElementById("filter_offline").onchange = function() {
		update_table();
	};
	document.getElementById("filter_dead").onchange = function() {
		update_table();
	};	
	document.getElementById("filter_collapsed").onchange = function() {
		update_table();
	};
	document.getElementById("game_selector").onchange = function() {
		update_game();
	};
});