<?php 

/**
  * Determine the webhook type and act accordingly
  */
function commerce_kindful_webhook() {
  $request = json_decode(file_get_contents('php://input'));
  watchdog('commerce_kindful', '<pre>' . print_r($request, true) . '</pre>');
  if(!empty($request)) {
    if(isset($request->data->object->id)) {
      if($request->type == "person.recurring_donation.remove") {
        $error = commerce_kindful_cancel_order($request);
      } elseif (!empty($request->data->object->cart->cart_items) || !empty($request->data->object->product)) {
        switch($request->type) {
          case "person.recurring_donation.create":
            $error = commerce_kindful_create_transaction($request);
            break;
          case "person.recurring_donation.update":
            $error = commerce_kindful_update_transaction($request);
            break;
          default:
            $error = "Error: invalid request type.";
        }
      } else {
        watchdog('commerce_kindful', "No products found for update with id !value.", array('!value' => $request->data->object->id), WATCHDOG_WARNING);
        $error = "Notice: Webhook Ignored. No products present.";
      }
    } else {
      watchdog('commerce_kindful', "Invalid request Kindful request", array(), WATCHDOG_WARNING);
      $error = "Error: Invalid request";
    }
    if($error) {
      print $error;
    }
    die;
  } else {
    watchdog('commerce_kindful', "No POST data found from Kindful request", array(), WATCHDOG_ERROR);
    print "Error: No POST data found".
    die;
  }
} 

/**
  * Look up a transaction by it's remote Kindful ID
  */
function commerce_kindful_get_payment_transaction($remote_id) {
  $query = new EntityFieldQuery;

  $result = $query
    ->entityCondition('entity_type', 'commerce_payment_transaction')
    ->propertyCondition('payment_method', 'kindful')
    ->propertyCondition('remote_id', $remote_id)
    ->execute();
  if (isset($result['commerce_payment_transaction']) && count($result['commerce_payment_transaction']) > 0) {
    $transaction = array_pop($result['commerce_payment_transaction']);
    return $transaction->transaction_id;
  }
  return FALSE;
}

/**
  * update or create new order and transaction for recurring donation
  */
function commerce_kindful_create_transaction($values) {
  // Make sure we aren't creating an order twice
  $transaction_id = commerce_kindful_get_payment_transaction($values->data->object->id);
  if (!empty($transaction_id)) {
    $response = commerce_kindful_update_transaction($values);
    return $response;
  }
  
  // Create a new order and associate a transaction with it
  $order = commerce_kindful_create_new_order($values);
  $transaction = commerce_payment_transaction_new('kindful', $order->order_id);
  
  // identifying data
  $transaction->instance_id = 'kindful|webhook_kindful';
  $transaction->remote_id = $values->data->object->id;
  // payment amount details
  $transaction->amount = $order->commerce_order_total[LANGUAGE_NONE][0]['amount'];
  $transaction->currency_code = $order->commerce_order_total[LANGUAGE_NONE][0]['currency_code'];
  // payment status
  $transaction->remote_status = 'true';
  $transaction->status = COMMERCE_PAYMENT_STATUS_SUCCESS;
  $transaction->message = t('Order completed');
  $transaction->payload = array(
    'order' => $order->order_id,
    'status' => 'success',
    'transaction_id' => $values->data->object->id,
    'response' => $values,
  );
  commerce_payment_transaction_save($transaction);
  commerce_kindful_decrease_stock_level($order);
  $order = commerce_order_status_update($order, 'completed');
  return FALSE; // no errrors
}

/**
  * If we could not find an order lets create a new one.
  */
function commerce_kindful_create_new_order($values) {
  // Create the new order in checkout; you might also check first to
  // see if your user already has an order to use instead of a new one.
  $order = commerce_order_new(0, 'pending');
  
  // Save the order to get its ID.
  commerce_order_save($order);
  $order_wrapper = entity_metadata_wrapper('commerce_order', $order);
  
  // Check for a cart else look for single product
  if(!empty($values->data->object->cart)) {
    foreach($values->data->object->cart->cart_items as $product_ref) {
      // Load whatever product represents the item the customer will be
      // paying for and create a line item for it.
      $product = commerce_product_load_by_sku($product_ref->product_id);
      if(!empty($product)) {
        $line_item = commerce_product_line_item_new($product, $product_ref->quantity, $order->order_id);
        if(empty($line_item)) {
          watchdog('commerce_kindful', "Could not create a new line item for product id !value.", array('!value' => $product_ref->product_id), WATCHDOG_WARNING);
        }
        // Save the line item to get its ID.
        commerce_line_item_save($line_item);
        $order_wrapper->commerce_line_items[] = $line_item;
      } else {
        watchdog('commerce_kindful', "Could not find product id !value.", array('!value' => $product_ref->product_id), WATCHDOG_WARNING);
        watchdog('commerce_kindful', '<pre>' . print_r($values, true) . '</pre>');
      }
    }
  } elseif(!empty($values->data->object->product->sku)) {
    // Load whatever product represents the item the customer will be
    // paying for and create a line item for it.
    $product = commerce_product_load_by_sku($values->data->object->product->sku);
    if(!empty($product)) {       
      $line_item = commerce_product_line_item_new($product, 1, $order->order_id);
      if(empty($line_item)) {
        watchdog('commerce_kindful', "Could not create a new line item for product id !value.", array('!value' => $values->data->object->product->sku), WATCHDOG_WARNING);
      }
      // Save the line item to get its ID.
      commerce_line_item_save($line_item);
      $order_wrapper->commerce_line_items[] = $line_item;
    } else {
      watchdog('commerce_kindful', "Could not find product id !value.", array('!value' => $values->data->object->product->sku), WATCHDOG_WARNING);
      watchdog('commerce_kindful', '<pre>' . print_r($values, true) . '</pre>');
    }
  } else {
    watchdog('commerce_kindful', "No valid products found for update with id !value.", array('!value' => $values->data->object->id), WATCHDOG_WARNING);
  }
  
  $order_wrapper->save();
  $order->data['schedule'] = $values->data->object->recurrence_period_option->occurs_on;
  
  
  // Save the order again to update its line item reference field.
  commerce_order_save($order);
  
  return $order;
}

/**
  * Update an exsisting order
  */
function commerce_kindful_update_transaction($values) {
  $transaction_id = commerce_kindful_get_payment_transaction($values->data->object->id);
  
  // if we don't find an order to update lets create a new one instead
  if (empty($transaction_id)) {
    $response = commerce_kindful_create_transaction($values);
    watchdog('commerce_kindful', "Could not find transaction for recurring donation with id !value. A new order has been created instead of an update.", array('!value' => $values->data->object->id), WATCHDOG_WARNING);
    $warning = "Warning: Could not find transaction for recurring donation with id " . $values->data->object->id . '. A new order has been created instead of an update. ';
    $response = ($response ? $warning . $response : $warning);
    return $response;
  }
  
  $transaction = commerce_payment_transaction_load($transaction_id);
  $order = commerce_order_load_by_number($transaction->order_id);
  
  // get array for current cart and updated cart for comparison 
  $current_cart = commerce_kindful_get_current_cart($order);
  $updated_cart = array();
  if(!empty($values->data->object->cart)) {
    $updated_cart = commerce_kindful_get_updated_cart($values->data->object->cart->cart_items);  
  } else {
    if(!empty($values->data->object->product->sku)) {
      $updated_cart[$values->data->object->product->sku] = array("quantity" => 1);
    }
  }
  
  // if products have been removed update order and restore stock
  $removed_products = array_diff(array_keys($current_cart), array_keys($updated_cart));
  if(!empty($removed_products)) {
    foreach($removed_products as $removed_product) {
      commerce_cart_order_product_line_item_delete($order, $current_cart[$removed_product]['line_item_id']);
      $product = commerce_product_load_by_sku($removed_product);
      if(!empty($product)) {
        commerce_kindful_stock_adjust($product, $current_cart[$removed_product]['quantity']);
      } else {
        watchdog('commerce_kindful', "Could not find product id !value on update remove product.", array('!value' => $removed_product), WATCHDOG_WARNING);
      }
    }
  }
  
  // if products have been added update order and reduce stock
  $added_products = array_diff(array_keys($updated_cart), array_keys($current_cart));
  if(!empty($added_products)) {
    $order_wrapper = entity_metadata_wrapper('commerce_order', $order);
    foreach($added_products as $added_product) {
      $product = commerce_product_load_by_sku($added_product);
      if(!empty($product)) {
        $line_item = commerce_product_line_item_new($product, $updated_cart[$added_product]['quantity'], $order->order_id);
        commerce_line_item_save($line_item);
        $order_wrapper->commerce_line_items[] = $line_item;
        commerce_kindful_stock_adjust($product, '-' . $updated_cart[$added_product]['quantity']);
      } else {
        watchdog('commerce_kindful', "Could not find product id !value on update add product.", array('!value' => $added_product), WATCHDOG_WARNING);
      }
    }
    $order_wrapper->save();
  }
  
  // get matching products and check for quantity updates
  $updated_products = array_intersect(array_keys($current_cart), array_keys($updated_cart));
  if(!empty($updated_products)) {
    foreach($updated_products as $updated_product) {
      if($current_cart[$updated_product]['quantity'] != $updated_cart[$updated_product]['quantity']) {
        // Update the line item on the order
        $line_item = commerce_line_item_load($current_cart[$updated_product]['line_item_id']);
        $line_item->quantity = $updated_cart[$updated_product]['quantity'];
        commerce_line_item_save($line_item);
        // update the products stock level
        $product = commerce_product_load_by_sku($updated_product);
        if(!empty($product)) {
          $quantity = $current_cart[$updated_product]['quantity'] - $updated_cart[$updated_product]['quantity'];
          commerce_kindful_stock_adjust($product, $quantity);
        } else {
          watchdog('commerce_kindful', "Could not find product id !value on update adjust quantity of product.", array('!value' => $added_product), WATCHDOG_WARNING);
        }
      }
    }
  }
  $order = commerce_order_status_update($order, 'completed');
  watchdog('commerce_kindful', "Order #!value has been updated via Kindful request.", array('!value' => $transaction->order_id), WATCHDOG_INFO);
  return FALSE; // no errrors
}

/**
  * return array of current products
  */
function commerce_kindful_get_current_cart($order) {
  $wrapper = entity_metadata_wrapper('commerce_order', $order);
  $products = array();
  foreach($wrapper->commerce_line_items as $delta => $line_item_wrapper) {
    if($line_item_wrapper->type->value() == 'product') {
      $product = $line_item_wrapper->commerce_product->value();
      $products[$product->sku] = array(
        "line_item_id" => $line_item_wrapper->line_item_id->value(),
        "quantity" => intval($line_item_wrapper->quantity->value()), 
      );
    }
  }
  return $products;
}

/**
  * return array of updated products
  */
function commerce_kindful_get_updated_cart($cart) {
  $products = array();
  foreach($cart as $product) {
    if(!empty($products[$product->product_id]['quantity'])) {
      $products[$product->product_id]['quantity'] = $products[$product->product_id]['quantity'] + intval($product->quantity);
    } else {
      $products[$product->product_id] = array(
        "quantity" => intval($product->quantity),
      );
    }
  }
  return $products;
}

/**
  * Cancel an order
  */
function commerce_kindful_cancel_order($values) {
  $transaction_id = commerce_kindful_get_payment_transaction($values->data->object->id);
  
  if (!empty($transaction_id)) {
    $transaction = commerce_payment_transaction_load($transaction_id);
    $order = commerce_order_load_by_number($transaction->order_id);
    if(!empty($order) && $order->status != 'canceled') {
      commerce_kindful_restore_stock_level($order);
      commerce_order_status_update($order, 'canceled');
      watchdog('commerce_kindful', "Order # !value has been canceled via Kindful request.", array('!value' => $transaction->order_id), WATCHDOG_INFO);
      return FALSE; // no errrors
    } else {
      watchdog('commerce_kindful', "Order cancel aborted. Order is already cancelled or not present with donation !value", array('!value' => $values->data->object->id), WATCHDOG_WARNING);
      return "Warning: Order cancel aborted. Order is already cancelled or not present with donation id " . $values->data->object->id . '.';
    }
  } else {
    watchdog('commerce_kindful', "Order cancel aborted. Could not find and remove transaction for recurring donation with id !value", array('!value' => $values->data->object->id), WATCHDOG_ERROR);
    return "Error: Delete Failed. Could not find transaction for recurring donation with id " . $values->data->object->id . '.';
  }
}

/**
  * When an order is cancelled restore stock to original level
  */
function commerce_kindful_restore_stock_level($order) {
  $wrapper = entity_metadata_wrapper('commerce_order', $order);
  
  // grab the product information
  foreach($wrapper->commerce_line_items as $delta => $line_item_wrapper) {
    if($line_item_wrapper->type->value() == 'product') {
      $product = $line_item_wrapper->commerce_product->value();
      commerce_kindful_stock_adjust($product, $line_item_wrapper->quantity->value());
    }
  }
}

/**
  * When an order is created decrease stock level
  */
function commerce_kindful_decrease_stock_level($order) {
  $wrapper = entity_metadata_wrapper('commerce_order', $order);
  
  // grab the product information
  foreach($wrapper->commerce_line_items as $delta => $line_item_wrapper) {
    if($line_item_wrapper->type->value() == 'product') {
      $product = $line_item_wrapper->commerce_product->value();
      commerce_kindful_stock_adjust($product, '-' . $line_item_wrapper->quantity->value());
    }
  }
}

/**
  * adjust stock level of a product
  */
function commerce_kindful_stock_adjust($product, $qty) {
  if (!commerce_stock_product_type_enabled($product->type)) {
    return;
  }

  $wrapper = entity_metadata_wrapper('commerce_product', $product);

  $new_stock = $wrapper->commerce_stock->value() + intval($qty);
  $wrapper->commerce_stock->set($new_stock);
  $result = $wrapper->save();

  if ($result) {
    watchdog('commerce_stock', 'Modified stock level of product %sku by %amount', array('%sku' => $product->sku, '%amount' => $qty));
  }
  else {
    watchdog('commerce_stock', 'Failed attempt to modify stock level of product %sku by %amount', array('%sku' => $product->sku, '%amount' => $qty), WATCHDOG_ERROR);
  }
}
