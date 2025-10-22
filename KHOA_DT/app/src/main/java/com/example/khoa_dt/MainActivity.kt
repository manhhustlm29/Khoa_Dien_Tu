package com.example.khoa_dt

import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import info.mqtt.android.service.MqttAndroidClient
import org.eclipse.paho.client.mqttv3.*

class MainActivity : AppCompatActivity() {

    private lateinit var mqttClient: MqttAndroidClient
    private val brokerUri = "tcp://broker.hivemq.com:1883"
    private val topic = "esp32/lock"

    private lateinit var tvStatus: TextView
    private lateinit var tvMessageLog: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvMessageLog = findViewById(R.id.tvMessageLog)

        // Kết nối MQTT ngay khi mở app
        connectMqtt()
    }

    private fun connectMqtt() {
        mqttClient = MqttAndroidClient(this, brokerUri, "AndroidClient_" + System.currentTimeMillis())

        val options = MqttConnectOptions().apply {
            isAutomaticReconnect = true
            isCleanSession = true
        }

        mqttClient.connect(options, null, object : IMqttActionListener {
            override fun onSuccess(asyncActionToken: IMqttToken?) {
                tvStatus.text = " Kết nối MQTT thành công!"
                subscribeTopic()
            }

            override fun onFailure(asyncActionToken: IMqttToken?, exception: Throwable?) {
                tvStatus.text = " Kết nối MQTT thất bại!"
            }
        })
    }

    private fun subscribeTopic() {
        mqttClient.subscribe(topic, 1, null, object : IMqttActionListener {
            override fun onSuccess(asyncActionToken: IMqttToken?) {
                // giữ nguyên TextView trên
            }

            override fun onFailure(asyncActionToken: IMqttToken?, exception: Throwable?) {}
        })

        mqttClient.setCallback(object : MqttCallback {
            override fun connectionLost(cause: Throwable?) {
                runOnUiThread { tvStatus.text = " Mất kết nối MQTT!" }
            }

            override fun messageArrived(topic: String?, message: MqttMessage?) {
                val msg = message.toString()
                runOnUiThread {
                    // Không thay đổi TextView trên, chỉ append log dưới
                    tvMessageLog.append(" Mở khóa: $msg\n")
                }
            }

            override fun deliveryComplete(token: IMqttDeliveryToken?) {}
        })
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            mqttClient.unregisterResources()
            mqttClient.close()
        } catch (_: Exception) { }
    }
}
