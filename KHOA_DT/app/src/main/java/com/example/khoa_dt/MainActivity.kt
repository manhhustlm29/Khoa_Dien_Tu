package com.example.khoa_dt

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import info.mqtt.android.service.MqttAndroidClient
import org.eclipse.paho.client.mqttv3.*

class MainActivity : AppCompatActivity() {

    private lateinit var mqttClient: MqttAndroidClient
    private val brokerUri = "tcp://test.mosquitto.org:1883"

    private val topicSub = "esp32/lock"
    private val topicPub = "esp32/rfid_add"   // gửi dữ liệu thô

    private lateinit var tvStatus: TextView
    private lateinit var tvMessageLog: TextView
    private lateinit var edtInput: EditText
    private lateinit var btnSend: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvMessageLog = findViewById(R.id.tvMessageLog)
        edtInput = findViewById(R.id.edtInput)
        btnSend = findViewById(R.id.btnSend)

        // ----- Gửi MQTT -----
        btnSend.setOnClickListener {
            val text = edtInput.text.toString().trim()
            if (text.isNotEmpty()) {
                sendMqttMessage(text)
                edtInput.setText("")       // <<< XOÁ NỘI DUNG NGAY SAU KHI GỬI
            }
        }

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
        mqttClient.subscribe(topicSub, 1, null, object : IMqttActionListener {
            override fun onSuccess(asyncActionToken: IMqttToken?) {}
            override fun onFailure(asyncActionToken: IMqttToken?, exception: Throwable?) {}
        })

        mqttClient.setCallback(object : MqttCallback {
            override fun connectionLost(cause: Throwable?) {
                runOnUiThread {
                    tvStatus.text = " Mất kết nối MQTT!"
                }
            }

            override fun messageArrived(topic: String?, message: MqttMessage?) {
                val msg = message.toString()
                runOnUiThread {
                    tvMessageLog.append(" Mở khóa: $msg\n")
                }
            }

            override fun deliveryComplete(token: IMqttDeliveryToken?) {}
        })
    }

    private fun sendMqttMessage(rawText: String) {
        // Gửi dữ liệu thô không JSON
        val message = MqttMessage(rawText.toByteArray())
        message.qos = 1
        mqttClient.publish(topicPub, message)
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            mqttClient.unregisterResources()
            mqttClient.close()
        } catch (_: Exception) { }
    }
}
