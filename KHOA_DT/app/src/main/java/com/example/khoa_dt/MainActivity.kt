package com.example.khoa_dt

import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.view.LayoutInflater
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.textfield.TextInputEditText
import info.mqtt.android.service.MqttAndroidClient
import org.eclipse.paho.client.mqttv3.*
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.*

class MainActivity : AppCompatActivity() {

    private lateinit var mqttClient: MqttAndroidClient
    private val brokerUri = "tcp://broker.hivemq.com:1883"

    // Topics
    private val topicLog = "esp32/lock"
    private val topicRfidCmd = "esp32/rfid_add"
    private val topicFingerCmd = "esp32/finger_add"
    private val topicAddResult = "esp32/rfid_add_result"

    // UI
    private lateinit var tvStatus: TextView
    private lateinit var imgStatusIcon: ImageView
    private lateinit var btnConnect: Button
    private lateinit var btnAddRfid: Button
    private lateinit var btnAddFinger: Button
    private lateinit var btnViewList: Button
    private lateinit var tvMessageLog: TextView

    private var isConnected = false
    private var pendingName: String = "" // Lưu tên tạm thời khi đang thêm

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        imgStatusIcon = findViewById(R.id.imgStatusIcon)
        btnConnect = findViewById(R.id.btnConnect)
        btnAddRfid = findViewById(R.id.btnAddRfid)
        btnAddFinger = findViewById(R.id.btnAddFinger)
        btnViewList = findViewById(R.id.btnViewList)
        tvMessageLog = findViewById(R.id.tvMessageLog)

        mqttClient = MqttAndroidClient(this, brokerUri, "AndroidApp_${System.currentTimeMillis()}")

        btnConnect.setOnClickListener { if (!isConnected) connectMqtt() else disconnectMqtt() }
        btnAddRfid.setOnClickListener { showAddDialog(isFinger = false) }
        btnAddFinger.setOnClickListener { showAddDialog(isFinger = true) }
        btnViewList.setOnClickListener { showUserList() }

        updateStatusUI(false)
    }

    // --- MQTT CONNECT ---
    private fun connectMqtt() {
        updateStatusUI(isConnecting = true)
        val options = MqttConnectOptions().apply { isAutomaticReconnect = true; isCleanSession = true }
        try {
            mqttClient.connect(options, null, object : IMqttActionListener {
                override fun onSuccess(t: IMqttToken?) {
                    isConnected = true
                    updateStatusUI(connected = true)
                    subscribeTopics()
                    appendLog("Sys", "Kết nối thành công!")
                }
                override fun onFailure(t: IMqttToken?, e: Throwable?) {
                    isConnected = false
                    updateStatusUI(connected = false)
                    appendLog("Err", "Lỗi kết nối: ${e?.message}")
                }
            })
            mqttClient.setCallback(object : MqttCallback {
                override fun connectionLost(c: Throwable?) { isConnected = false; updateStatusUI(false) }
                override fun messageArrived(t: String?, m: MqttMessage?) {
                    runOnUiThread { handleIncomingMessage(t, m.toString()) }
                }
                override fun deliveryComplete(t: IMqttDeliveryToken?) {}
            })
        } catch (e: Exception) { e.printStackTrace() }
    }

    private fun disconnectMqtt() {
        try { mqttClient.disconnect(); isConnected=false; updateStatusUI(false) } catch(e:Exception){}
    }

    private fun subscribeTopics() {
        mqttClient.subscribe(topicLog, 1)
        mqttClient.subscribe(topicAddResult, 1)
    }

    // --- LOGIC XỬ LÝ TIN NHẮN ---
    private fun handleIncomingMessage(topic: String?, message: String) {
        if (topic == topicAddResult) {
            try {
                val json = JSONObject(message)
                val status = json.optString("status")

                // Các bước hướng dẫn vân tay
                if (status == "step1") Toast.makeText(this, "Đặt ngón tay lên cảm biến...", Toast.LENGTH_SHORT).show()
                else if (status == "step2") Toast.makeText(this, "Nhấc tay ra...", Toast.LENGTH_SHORT).show()
                else if (status == "step3") Toast.makeText(this, "Đặt lại lần 2...", Toast.LENGTH_SHORT).show()

                // Kết quả thành công
                else if (status == "success") {
                    val type = json.optString("type")
                    val id = if(type=="finger") json.optString("id") else json.optString("uid")

                    // Logic lưu Database
                    if (pendingName.isNotEmpty()) {
                        val key = if(type=="finger") "FINGER_$id" else id
                        saveUserToDatabase(key, pendingName)
                        appendLog("Add", "Đã thêm $pendingName ($key)")
                        Toast.makeText(this, "Thêm thành công!", Toast.LENGTH_LONG).show()
                        pendingName = ""
                    }
                }
                else if (status == "exist") Toast.makeText(this, "Đã tồn tại!", Toast.LENGTH_SHORT).show()
                else if (status == "full") Toast.makeText(this, "Bộ nhớ đầy!", Toast.LENGTH_SHORT).show()
                else if (status == "fail") Toast.makeText(this, "Thất bại: ${json.optString("msg")}", Toast.LENGTH_SHORT).show()

            } catch (e: Exception) { appendLog("Err", "JSON Error: $message") }
        } else if (topic == topicLog) {
            // Log mở cửa: Thay thế ID bằng Tên
            var finalMsg = message

            // Regex Vân tay: FINGER 123
            val fMatch = Regex("FINGER (\\d+)").find(message)
            if (fMatch != null) {
                val fid = fMatch.groupValues[1]
                val name = getUserName("FINGER_$fid")
                finalMsg = message.replace("FINGER $fid", "Vân tay $name ($fid)")
            }
            // Regex RFID: RFID A1B2
            val rMatch = Regex("RFID ([0-9A-F]+)").find(message)
            if (rMatch != null) {
                val uid = rMatch.groupValues[1]
                val name = getUserName(uid)
                finalMsg = message.replace("RFID $uid", "Thẻ $name ($uid)")
            }
            appendLog("Unlock", finalMsg)
        }
    }

    // --- UI DIALOGS ---
    private fun showAddDialog(isFinger: Boolean) {
        if (!isConnected) { Toast.makeText(this, "Chưa kết nối!", Toast.LENGTH_SHORT).show(); return }
        val view = LayoutInflater.from(this).inflate(R.layout.dialog_add_rfid, null)
        val input = view.findViewById<TextInputEditText>(R.id.edtRfidName)

        AlertDialog.Builder(this)
            .setTitle(if(isFinger) "Thêm Vân Tay" else "Thêm Thẻ RFID")
            .setView(view)
            .setPositiveButton("Bắt đầu") { _, _ ->
                val name = input.text.toString().trim()
                if (name.isNotEmpty()) {
                    pendingName = name
                    val topic = if(isFinger) topicFingerCmd else topicRfidCmd
                    val cmd = if(isFinger) "CMD_ADD_FINGER" else "CMD_ADD"
                    try { mqttClient.publish(topic, MqttMessage(cmd.toByteArray())) } catch(e:Exception){}
                    appendLog("Info", "Đang chờ quét cho: $name...")
                }
            }
            .setNegativeButton("Hủy", null).show()
    }

    private fun showUserList() {
        val pref = getSharedPreferences("UserDB", Context.MODE_PRIVATE)
        val all = pref.all
        if(all.isEmpty()) { Toast.makeText(this, "Danh sách trống", Toast.LENGTH_SHORT).show(); return }

        val sb = StringBuilder()
        for ((k, v) in all) sb.append("- $v [$k]\n")

        AlertDialog.Builder(this).setTitle("Danh sách (${all.size})")
            .setMessage(sb.toString())
            .setNeutralButton("Xóa Hết") {_,_-> pref.edit().clear().apply(); Toast.makeText(this,"Đã xóa DB",Toast.LENGTH_SHORT).show()}
            .setPositiveButton("Đóng", null).show()
    }

    // --- DATABASE HELPER ---
    private fun saveUserToDatabase(key: String, name: String) {
        getSharedPreferences("UserDB", Context.MODE_PRIVATE).edit().putString(key, name).apply()
    }
    private fun getUserName(key: String): String {
        return getSharedPreferences("UserDB", Context.MODE_PRIVATE).getString(key, "Người lạ") ?: "Người lạ"
    }

    // --- UI HELPER ---
    private fun updateStatusUI(connected: Boolean = false, isConnecting: Boolean = false) {
        runOnUiThread {
            if (isConnecting) {
                tvStatus.text = "Đang kết nối..."
                // SỬA DÒNG NÀY: Thay Color.ORANGE bằng Color.parseColor("#FFA500")
                tvStatus.setTextColor(Color.parseColor("#FFA500"))
                btnConnect.isEnabled = false
                return@runOnUiThread
            }
            if (connected) {
                tvStatus.text = "Online"
                tvStatus.setTextColor(Color.GREEN)
                imgStatusIcon.setColorFilter(Color.GREEN)
                btnConnect.text = "Ngắt kết nối"
                btnAddRfid.isEnabled = true
                btnAddFinger.isEnabled = true
            } else {
                tvStatus.text = "Offline"
                tvStatus.setTextColor(Color.RED)
                imgStatusIcon.setColorFilter(Color.RED)
                btnConnect.text = "Kết nối"
                btnAddRfid.isEnabled = false
                btnAddFinger.isEnabled = false
            }
        }

    }

    private fun appendLog(tag: String, msg: String) {
        runOnUiThread {
            val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
            tvMessageLog.text = "[$time] $tag: $msg\n${tvMessageLog.text}"
        }
    }
}