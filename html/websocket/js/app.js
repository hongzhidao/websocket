
$(document).ready(function() {

	var host = window.location.host;

	var v = "ws://" + host + "/ws?group_id=xyz";
	$('#location').val(v);

	var websocket;

	$('#connect').click(function() {
		var url = $('#location').val();
		if (url) {
			websocket = new WebSocket(url);

    		websocket.onopen = function(evt) { 
				$('#log').append("<li>CONNECTED</li>");

				$('#connect').attr('disabled', 'disabled');
				$('#disconnect').removeAttr('disabled');
				$('#message').removeAttr('disabled');
				$('#send').removeAttr('disabled');
			}

    		websocket.onmessage = function(evt) { 
				$('#log').append("<li class='y'>RESPONSE:" + evt.data + "</li>");
			}

    		websocket.onerror = function(evt) { 
				$('#log').append("<li class='x'>ERROR:" + evt.data + "</li>");
			}

    		websocket.onclose = function(evt) { 
				$('#log').append("<li>CLOSED</li>");

				$('#connect').removeAttr('disabled');
				$('#disconnect').attr('disabled', 'disabled');
				$('#message').attr('disabled', 'disabled');
				$('#send').attr('disabled', 'disabled');

				websocket.close();
			}
		}
	});

	$('#send').click(function() {
		var message = $('#message').val();
		if (message) {
			$('#log').append("<li>SENT:" + message + "</li>");
    		websocket.send(message);
		}
	});

	$('#disconnect').click(function() {
		$('#log').append("<li>DISCONNECTED</li>");

		$('#connect').removeAttr('disabled');
		$('#disconnect').attr('disabled', 'disabled');
		$('#message').attr('disabled', 'disabled');
		$('#send').attr('disabled', 'disabled');

		websocket.close();
	});

	$('#clear').click(function() {
		$('#log').html('');
	});

});

