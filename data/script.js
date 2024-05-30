$( document ).ready(function() {
    init_socket();
    dashboard();
});

function dashboard()
{
    $.get("dashboard.html", function(data){
        $("#main_content").html(data);
    })
}

function wifi()
{
    $.get("wifi.html", function(data){
        $("#main_content").html(data);
    })
}

function init_socket()
{
    console.log("Initilizing web sockets.")
    Socket = new WebSocket('ws://'+window.location.hostname+"/ws");
    Socket.onmessage = function(event){
        console.log("Web socket message recieved...")
    }
    Socket.onopen = function(event){
        console.log("Connected to web sockets...")
    }
    Socket.onclose = function(event){
        console.log("Connection to websockets closed....")
        setTimeout(function() {
            console.log("Retrying websocket connection....")
            init_socket();
        }, 1000);
    }
    Socket.onerror = function(event){
        console.log("Error in websockets");
    }
}